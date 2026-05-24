# Page Fault Tracer (pft)

Учебный модуль ядра Linux, который трейсит page fault'ы на x86 и
отдаёт статистику в userspace.

## Что делает

На каждый page fault в системе модуль:

1. Инкрементирует глобальные atomic-счётчики (total, user, kernel,
   write, эвристика CoW).
2. Записывает фиксированной длины бинарную запись в кольцевой буфер
   `kfifo`.
3. Обновляет per-PID hash-таблицу (счётчик + последнее `comm`).

Userspace может:

- читать сырой поток событий из `/sys/kernel/debug/pft/events`,
- читать агрегированную статистику и топ-PID'ов из `/proc/pft/stats`,
- отправлять команды в `/proc/pft/control` (reset, поставить/снять
  PID-фильтр).

В комплекте с модулем поставляются маленький reader (`pft-ctl`) и
микробенч (`pft-bench`).

## Архитектура

```
   userspace
   ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
   │ pft-ctl      │     │ cat /proc/   │     │ echo cmd >   │
   │ (reader)     │     │ pft/stats    │     │ /proc/pft/   │
   │              │     │              │     │ control      │
   └──────┬───────┘     └──────┬───────┘     └──────┬───────┘
          │ read()             │ read()             │ write()
   ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┼ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─
          ▼                    ▼                    ▼
   ┌────────────────────────────────────────────────────────┐
   │  pft.ko                                                │
   │                                                        │
   │  /sys/kernel/debug/pft/events  -> kfifo<pft_event>     │
   │  /proc/pft/stats               -> seq_file show        │
   │  /proc/pft/control             -> command parser       │
   │                                                        │
   │  per-PID hash (DEFINE_HASHTABLE, spinlock)             │
   │  atomic64 counters + atomic filter_pid                 │
   │                                                        │
   │           ▲                                            │
   │           │ called from probe context (atomic)         │
   │  ┌────────┴─────────┐                                  │
   │  │ probe functions  │  registered via                  │
   │  │  user / kernel   │  tracepoint_probe_register()     │
   │  └────────▲─────────┘  on tracepoints discovered by    │
   │           │            for_each_kernel_tracepoint()    │
   └───────────┼────────────────────────────────────────────┘
               │
   ────────────┼──────────── kernel core ────────────────────
               │
        exceptions:page_fault_user
        exceptions:page_fault_kernel
        (defined in arch/x86/mm/fault.c)
```

## Почему такие технические решения

**Tracepoint'ы, а не kprobe и не подмена syscall'ов.**
`exceptions:page_fault_user` / `page_fault_kernel` — это статические
tracepoint'ы со стабильной сигнатурой: `(unsigned long address,
struct pt_regs *regs, unsigned long error_code)`. Они являются частью
ABI ядра для observability-инструментов: не переезжают между версиями,
как переменчивые kprobe-таргеты, и почти ничего не стоят, когда никто
не подписан (один static-branch).

**Регистрация по имени, а не через `register_trace_*()`.**
В `arch/x86/mm/fault.c` через `CREATE_TRACE_POINTS` создаётся символ
`__tracepoint_page_fault_user`, но `EXPORT_TRACEPOINT_SYMBOL_GPL()`
для него **не вызывается**. Поэтому out-of-tree модуль, который
раскрывает макрос `register_trace_page_fault_user()`, не линкуется:
`modpost: __tracepoint_page_fault_user undefined`. Стандартный обход
(его же применяют bpftrace и SystemTap) — пройти список всех
tracepoint'ов ядра через `for_each_kernel_tracepoint()`, найти по имени
и зарегистрироваться через `tracepoint_probe_register(tp, probe, NULL)`.
Оба helper'а экспортированы как `EXPORT_SYMBOL_GPL`.

**Дисциплина atomic-контекста.**
Probe-функции вызываются с выключенным preempt'ом и внутри
RCU read-side critical section. Поэтому в коде:

- никаких sleep'ов — нет mutex'ов, нет `down_*`, нет `GFP_KERNEL`;
- только `atomic64_inc`, `kfifo_in_spinlocked`, `kmalloc(GFP_ATOMIC)`,
  спинлоки (`spin_lock_irqsave`);
- никакого `copy_to_user` из probe — весь обмен с userspace
  происходит в process context внутри `pft_events_read`.

**Per-PID hash с double-checked insertion.**
Аллокация записи для нового PID обязана происходить **вне** удержанного
спинлока (под спинлоком с IRQ-save `kmalloc(GFP_ATOMIC)` ненадёжен в
ряде конфигов). Поэтому probe делает так:

1. Lookup под локом; если нашли — инкремент и выход.
2. Отпускаем лок; `kmalloc(GFP_ATOMIC)`.
3. Берём лок снова; **повторно проверяем**, не вставил ли этот PID
   другой CPU за время нашей аллокации; если успел — отбрасываем нашу
   заготовку. Иначе вставляем.

Это классический double-checked locking; без повторной проверки гонка
с probe на другом CPU породила бы дубликат.

**Две файловые системы — две роли.**
`procfs` несёт стабильные, человекочитаемые поверхности (`stats`,
`control`) — то, что хотел бы `cat`-нуть админ. `debugfs` несёт сырой
бинарный поток событий (`events`) — то, что потребляет debug- или
profiling-инструмент. Такое разделение идиоматично для современного
ядра.

**Строгий порядок teardown в `pft_exit`.**

```
   unregister probes
   tracepoint_synchronize_unregister()   <-- ждём завершения in-flight probe'ов
   proc_remove(...)
   debugfs_remove_recursive(...)
   pft_hash_drain()
```

Если убрать debugfs-файл до того, как in-flight probe'ы завершатся, —
probe, проснувшийся на другом CPU, может вызвать
`wake_up_interruptible(&pft_wq)` уже после того, как очередь ожидания
ломается другим reader'ом. Если дренировать hash до synchronize —
probe может вставить запись в полуосвобождённую таблицу. Паттерн
«остановить producer'а → дождаться, пока producer затихнет → разобрать
всё остальное» — единственный безопасный порядок.

## Интерфейсы

### `/sys/kernel/debug/pft/events` (права 0400, только read)

Поток фиксированных записей `struct pft_event` по 48 байт:

| смещение | размер | поле     | тип |
|----------|--------|----------|------|
|  0       |  8     | ts_ns    | u64 (CLOCK_MONOTONIC, ns) |
|  8       |  4     | pid      | u32  |
| 12       |  4     | tgid     | u32  |
| 16       | 16     | comm     | char[TASK_COMM_LEN] |
| 32       |  8     | addr     | u64 (фолтовый VA) |
| 40       |  4     | err_code | u32 (биты X86_PF_*) |
| 44       |  4     | _pad     | u32  |

Read блокирующий по умолчанию; поддерживает `O_NONBLOCK` и
`poll(POLLIN)`.

### `/proc/pft/stats` (права 0444, текст)

```
total            <всего фолтов>
user             <user-mode фолтов>
kernel           <kernel-mode фолтов>
write            <write-фолтов>
cow_hint         <эвристика CoW>
dropped_fifo     <событий потеряно — fifo был полон>
dropped_pid_oom  <per-PID вставок не удалось из-за GFP_ATOMIC>
filter_pid       <N>   (0 = отключён)
unique_pids      <N>

top pids:
   pid   count comm
   ...
```

### `/proc/pft/control` (права 0200, только write)

Одна команда на одну запись:

- `reset` — обнулить все счётчики и очистить per-PID hash.
- `filter pid N` — оставить только события с `current->pid == N`.
- `filter clear` (или `filter off`) — выключить PID-фильтр.

```sh
echo "filter pid 42" > /proc/pft/control
echo reset           > /proc/pft/control
echo "filter clear"  > /proc/pft/control
```

## Расшифровка `err_code`

| бит | имя           | 0 значит        | 1 значит               |
|-----|---------------|-----------------|------------------------|
|  0  | `X86_PF_PROT`  | not-present     | страница есть, нарушение защиты |
|  1  | `X86_PF_WRITE` | read            | write                  |
|  2  | `X86_PF_USER`  | kernel-mode     | user-mode (CPL=3)      |
|  4  | `X86_PF_INSTR` | data access     | instruction fetch      |

Типичные значения, которые видно на busybox:

| `ec`  | смысл                                                       |
|-------|-------------------------------------------------------------|
| 0x04  | user, read, страницы нет → ленивая подкачка                 |
| 0x06  | user, write, нет → первая запись после `malloc`/`mmap`      |
| 0x07  | user, write, страница есть, не writable → **copy-on-write** |
| 0x14  | user, instruction fetch, нет → подгрузка `.text` страницы   |

## Сборка и запуск

На Linux-хосте для сборки:

```sh
cd pft
make            # собирает pft.ko, pft-ctl, pft-bench
make install    # копирует их + demo.sh в ../root/
# пересобрать initramfs.gz и запустить QEMU как обычно
```

В QEMU shell:

```sh
mount -t debugfs none /sys/kernel/debug 2>/dev/null
insmod /pft.ko
/demo.sh                    # быстрая end-to-end демонстрация
cat /proc/pft/stats         # агрегированный вид
/pft-ctl                    # читать поток событий
```

## Сценарий для защиты

1. `insmod /pft.ko` → показать `dmesg | tail`.
2. `/demo.sh` → полная картина: stats, top PIDs, примеры событий.
3. Показать CoW: запустить `/pft-bench 32`, спровоцировать fork в
   shell'е, наблюдать рост `cow_hint`.
4. Показать фильтр:
   ```sh
   /pft-bench 100 &
   echo "filter pid $!" > /proc/pft/control
   wait
   cat /proc/pft/stats           # только этот PID в статистике
   echo "filter clear" > /proc/pft/control
   ```
5. Измерить overhead через `/pft-bench`:
   ```sh
   rmmod pft                     # базовая линия
   /pft-bench 10000 5
   insmod /pft.ko
   /pft-bench 10000 5            # с трейсером
   ```
   Сравнить per-fault микросекунды.
6. `rmmod pft` → `dmesg | tail` покажет итоговые счётчики; объяснить
   безопасную выгрузку через `tracepoint_synchronize_unregister()`.

## Известные ограничения

- **Только x86.** Page-fault tracepoint'ы, которые мы потребляем,
  объявлены в `arch/x86/mm/fault.c`. На других архитектурах page fault
  экспонируется иначе (kprobe на `handle_mm_fault`, perf SW events).
  Выбор одного стабильного интерфейса — сознательный.
- **Minor vs major — эвристика.** Tracepoint срабатывает до того, как
  ядро решает, делать ли I/O с диска (`do_swap_page`, `filemap_fault`).
  Для точного разделения нужно ещё подцепиться к одному из этих мест
  или использовать perf SW counters
  `PERF_COUNT_SW_PAGE_FAULTS_{MIN,MAJ}`.
- **Детектор CoW — эвристика.** Комбинация `X86_PF_WRITE | X86_PF_PROT`
  одновременно покрывает CoW после `fork()` *и* write-после-`mprotect`.
  Поэтому счётчик называется `cow_hint`, а не `cow_count` — это честно.
- **Переиспользование PID не отслеживается.** Когда PID освобождается
  после завершения процесса, старая запись в hash живёт до reset.
  Для учебного модуля это нормально; в production-инструменте надо
  было бы подцепиться к `sched_process_exit`.
- **При burst-нагрузке буфер теряет события.** Kfifo вмещает 1024
  записи; пики свыше этого инкрементируют `dropped_fifo`. Размер —
  trade-off между памятью и точностью захвата.

## Состав каталога

```
pft/
├── Makefile         сборка модуля и userspace-утилит
├── pft.c            модуль ядра
├── pft-ctl.c        userspace reader событий
├── pft-bench.c      синтетический генератор page fault'ов
├── demo.sh          одноразовая демонстрация
└── README.md        этот файл
```
