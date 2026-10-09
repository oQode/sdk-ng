# Zephyr SDK 1.0.1 с потоками в libstdc++ (`cpp_gthreads`)

Форк [zephyrproject-rtos/sdk-ng](https://github.com/zephyrproject-rtos/sdk-ng) от тега `v1.0.1`.
Описание исходного SDK — в [README upstream](https://github.com/zephyrproject-rtos/sdk-ng/blob/v1.0.1/README.md).
Цель — тулчейн `arm-zephyr-eabi` для Linux x86_64, в котором libstdc++ собрана с моделью потоков `posix`
и TLS, без аварийного пула исключений и с исключениями в `-Os`-варианте библиотек.

Статус: патчи и изменения сделаны, тулчейн ещё не собирался.

## Зачем

Zephyr SDK собирает GCC с `--enable-threads=no`. В `c++config.h` нет `_GLIBCXX_HAS_GTHREADS` и `_GLIBCXX_HAVE_TLS`,
поэтому в многопоточном приложении:

- счётчики ссылок `std::shared_ptr` не атомарные: `__default_lock_policy = _S_single`, а `__is_single_threaded()`
  без `__GTHREADS` всегда `true`, так что не помогает и `_S_atomic`;
- состояние исключений (`__cxa_get_globals()`) одно на все потоки: если поток заблокирован внутри `catch`,
  а другой бросает и ловит своё исключение, `__cxa_end_catch`/`throw;` берут чужое исключение;
- `__cxa_guard_acquire` ничего не блокирует: второй поток во время инициализации function-local static получает
  `__gnu_cxx::recursive_init_error`, в узком окне инициализация выполняется дважды;
- мьютекс аварийного пула исключений ничего не делает;
- нет `std::thread`, `std::mutex`, `std::condition_variable`, `std::call_once`, `std::future`.

Upstream поддержку один раз включил (`--enable-threads=c11`, SDK 0.16.6, sdk-ng#735) и через неделю откатил
(sdk-ng#753): однопоточные C++-приложения перестали линковаться. Повторные попытки (sdk-ng#774, zephyr#43729)
закрыты ботом, sdk-ng#1142 (`--enable-threads=posix`) получил NAK, потому что C++ в Zephyr не должен зависеть от POSIX.
Здесь эта зависимость принимается сознательно.

## Ветки

- `oQode/sdk-ng`, ветка `cpp_gthreads` от `v1.0.1` (`cb61ae1`).
- `oQode/gcc`, ветка `cpp_gthreads` от `zephyrproject-rtos/gcc@a165a0c` (сабмодуль `gcc` в `v1.0.1`).
  Сабмодуль `gcc` в этом репозитории переключается на форк.
- crosstool-ng и picolibc не форкаются: заглушка `pthread.h` кладётся шагом CI.

## Патчи в `oQode/gcc`

1. **Мьютексы** (`libgcc/gthr-posix.h`). Убрать `#define __GTHREAD_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER`,
   оставить `__GTHREAD_MUTEX_INIT_FUNCTION`. Иначе `std::mutex` инициализируется константой и не имеет деструктора
   (`bits/std_mutex.h`), а в Zephyr `PTHREAD_MUTEX_INITIALIZER` (`-1`) при первом `lock()` занимает слот пула
   `CONFIG_MAX_PTHREAD_MUTEX_COUNT` и никогда его не отдаёт. С патчем конструктор вызывает `pthread_mutex_init`,
   деструктор — `pthread_mutex_destroy`. Condvar и рекурсивный мьютекс уже уничтожаются, их не трогать.
   Патч безусловный: тулчейн только для Zephyr, а `__ZEPHYR__` компилятор сам не определяет.
   Следствие: `std::mutex` теряет `constexpr`-конструктор, `constinit std::mutex` не компилируется.
2. **`-Os` с исключениями** (`config-ml.in`). Для `@Os`-multilib сейчас добавляется
   `-fno-exceptions -fno-asynchronous-unwind-tables`; убрать `-fno-exceptions`. Тогда библиотека, которую драйвер
   выбирает по `-Os` на строке линковки, бросает исключения, а не вызывает `abort()`.
3. **TLS** (`libstdc++-v3/configure.ac`, `configure`). Нужен `_GLIBCXX_HAVE_TLS`, иначе `eh_globals` хранятся
   в `pthread_key`, а Zephyr `pthread_setspecific` в потоках, созданных не через `pthread_create` (main, workqueue,
   BT RX), возвращает `EINVAL`, и libsupc++ вызывает `std::terminate`. SDK собирается с `--with-newlib`, в этой ветке
   configure не вызывает `GCC_CHECK_TLS`, поэтому `gcc_cv_have_tls` не помогает: `HAVE_TLS` задаётся для
   `arm*-zephyr-*` рядом с `*-rtems*`.
4. **Время** (`libstdc++-v3/acinclude.m4`, `configure`). `--enable-libstdcxx-time=yes` решает по link-тестам,
   а они не проходят: `clock_gettime`, `nanosleep`, `sched_yield` реализует Zephyr, а не picolibc. В режиме `auto`
   для `*-zephyr-*` они заданы явно, `steady_clock` идёт в `clock_gettime(CLOCK_MONOTONIC)`, `sleep_for` — в `nanosleep`.
   picolibc объявляет `CLOCK_MONOTONIC` только при `_POSIX_MONOTONIC_CLOCK`, поэтому libstdc++ собирается с ним
   (см. `CT_CC_GCC_ENABLE_CXX_FLAGS` ниже).
5. **Сильные ссылки на pthread** (`libstdc++-v3/config/os/newlib/os_defines.h`). По умолчанию
   `_GLIBCXX_GTHREAD_USE_WEAK 1`, и `__gthread_active_p()` проверяет weak-ссылку на `pthread_cancel`: если она
   не слинкована, `std::mutex`, guard'ы статических переменных и `shared_ptr` молча работают без блокировок.
   С `_GLIBCXX_GTHREAD_USE_WEAK 0` `__gthread_active_p()` всегда 1, а отсутствие pthread — ошибка линковки.
6. **Список multilib** (`gcc/config/arm/t-m4f-m33f`, `--with-multilib-list=@t-m4f-m33f`). Из `rmprofile` оставлены
   только `thumb/v7e-m+fp/hard` (Cortex-M4F, nRF52840) и `thumb/v8-m.main+fp/hard` (Cortex-M33F, nRF54L15) и их
   `space`: 6 вариантов вместо 68, сборка укладывается в лимит job GitHub. Имена каталогов и сопоставление опций
   как в `rmprofile`. Другие ядра и soft-float (`CONFIG_FPU=n`) молча получают multilib по умолчанию (ARM-режим,
   soft-float), то есть не поддерживаются.

Сгенерированный `configure` правится вручную синхронно с `.ac`/`.m4` (autoconf 2.69 не нужен).

## Изменения в этом репозитории

1. `configs/arm-zephyr-eabi.config`:
   - `CT_CC_GCC_MULTILIB_LIST="@t-m4f-m33f"` (патч 6);
   - `CT_CC_GCC_ENABLE_CXX_FLAGS="-D_POSIX_MONOTONIC_CLOCK=200112L"` — только для компиляции libstdc++ (патч 4),
     в установленные заголовки не попадает; значение `CLOCK_MONOTONIC` (4) совпадает с Zephyr;
   - `CT_CC_GCC_EXTRA_CONFIG_ARRAY` — опции из `common.config` и:
   - `--enable-threads=posix` (crosstool-ng для bare-metal ставит `--enable-threads=no`, пользовательские опции
     идут после и перекрывают);
   - `--enable-libstdcxx-static-eh-pool --with-libstdcxx-eh-pool-obj-count=0` (без аварийного пула, см. ниже).

   Не в `common.config`: TLS в патче GCC включён только для `arm*-zephyr-*`, другая цель с этими опциями получила бы
   потоки без TLS.
2. `stubs/pthread.h` — типы и инициализаторы копией из Zephyr версии NCS в `ncs_version`
   (`include/zephyr/posix/posix_types.h`, `pthread.h`): они компилируются в `libstdc++.a` и должны совпадать с ABI
   Zephyr. Функции — только те, что использует `gthr-posix.h` и реализует `lib/posix/options`.
   - В сборке Zephyr используется настоящий заголовок: напрямую, если `CONFIG_POSIX_SYSTEM_INTERFACES` ставит
     `include/zephyr/posix` первым в путь, иначе через `__has_include(<zephyr/posix/pthread.h>)` в заглушке.
   - Read-write lock объявлены только для C (нужны C-части `gthr-posix.h`). В C++ их нет, configure libstdc++ не
     находит `pthread_rwlock_t`, и `std::shared_mutex` собирается на condvar: вариант на `pthread_rwlock_t` берёт
     `PTHREAD_RWLOCK_INITIALIZER` без `pthread_rwlock_destroy` и терял бы слот пула, как `std::mutex` без патча 1.
   - Заглушка из sdk-ng#1142 не подходит: её `{0}`-инициализаторы Zephyr считает невалидным объектом (`EINVAL`).
   - crosstool-ng (`picolibc_headers`) копирует весь `picolibc/newlib/libc/include/` в sysroot, поэтому CI кладёт
     заглушку туда до сборки.
3. `ncs_version` — версия NCS, под которую собран SDK; попадает в SDK файлом-маркером `ncs_version`.
4. `.github/workflows/cpp_gthreads.yml` вместо upstream `ci.yml`, `release.yml`, `twister.yml` (им нужны
   self-hosted runner'ы и секреты Zephyr). Хост `linux-x86_64` на `ubuntu-24.04`, цель `arm-zephyr-eabi`.
   Запуск вручную (`workflow_dispatch`) — artifact; тег `cpp_gthreads-1.0.1-N` — ещё и GitHub release.
   SDK: `sdk_version`, `sdk_gnu_toolchains`, `ncs_version`, `cmake/`, `gnu/arm-zephyr-eabi`, без host tools.
   `sdk_version` остаётся `1.0.1`, иначе `find_package(Zephyr-sdk 1.0)` в Zephyr не найдёт SDK.
5. `scripts/check_cpp_gthreads.sh <gnu/arm-zephyr-eabi>` — проверки после сборки в CI, для
   `thumb/v7e-m+fp/hard`, `thumb/v8-m.main+fp/hard` и их `space`:
   - `arm-zephyr-eabi-gcc -v` → `Thread model: posix`;
   - Cortex-M4F и Cortex-M33F с `-O2`/`-Os` выбирают ожидаемый каталог multilib;
   - компиляция `<mutex>`: есть `_GLIBCXX_HAS_GTHREADS`, `_GLIBCXX_HAVE_TLS`, `_GLIBCXX_USE_CLOCK_MONOTONIC`,
     `_GLIBCXX_USE_NANOSLEEP`; `_GLIBCXX_USE_CXX11_ABI` — 1, `_GLIBCXX_GTHREAD_USE_WEAK` — 0; нет
     `__GTHREAD_MUTEX_INIT` и `_GLIBCXX_USE_PTHREAD_RWLOCK_T`; у `std::mutex` есть деструктор;
   - `libstdc++.a`: нет `emergency_pool`;
     `functexcept.o` вызывает `__cxa_throw`, `guard.o` — `pthread_once`, в `eh_globals.o` есть TLS-символ.

   На SDK 1.0.1 скрипт проходит только `functexcept.o` без `-Os`.

## COW-строки

Остаются, dual ABI включён, как в SDK 1.0.1. В `std::runtime_error` и других исключениях хранится `__cow_string`,
поэтому в прошивку попадают обе реализации строк: `cow-string-inst.o` (~5,3 КБ) и обычная `string-inst.o`
(~5,2 КБ), плюс `cow-stdexcept.o` (~2,4 КБ).

Убрать их опциями нельзя: `--disable-libstdcxx-dual-abi` в GCC 14 оставляет только старый ABI
(`default_libstdcxx_abi="gcc4-compatible"`, `--with-default-libstdcxx-abi` игнорируется), то есть COW-строки
везде. Сборка «только новый ABI» upstream не поддерживается и потребовала бы патчей configure и исходников
libstdc++.

## Аварийный пул исключений

`__cxa_allocate_exception()` выделяет память под объект исключения и заголовок (на ARM 248 Б вместе с
`__cxa_dependent_exception`, в основном `_Unwind_Control_Block`) через `malloc`; если `malloc` вернул `NULL` —
из пула; если и пул пуст — `std::terminate()`. Пул нужен, чтобы можно было бросить `std::bad_alloc` при исчерпанной
куче; для `std::runtime_error("...")` он почти бесполезен, строка сообщения выделяется до `throw`.

В SDK 1.0.1 пул динамический: статический конструктор читает `getenv("GLIBCXX_TUNABLES")` и делает
`malloc(4 × (6·4 + 248)) = 1088 Б` на всё время работы, код пула ~0,35 КБ ROM. С `__GTHREADS` число объектов
по умолчанию `4·4·4 = 64`, то есть 17 408 Б кучи при старте, поэтому размер задаётся явно.

Выбрано `--enable-libstdcxx-static-eh-pool --with-libstdcxx-eh-pool-obj-count=0`: `USE_POOL=0`, кода пула нет,
`getenv` и `malloc` при старте нет. Экономия 1088 Б кучи и ~0,35 КБ ROM. `throw` при нехватке памяти —
`std::terminate`.

## Что нужно приложению

- `ZEPHYR_SDK_INSTALL_DIR` указывает на этот SDK (окружение NCS по умолчанию указывает на встроенный).
- `CONFIG_POSIX_API=y`, `CONFIG_THREAD_LOCAL_STORAGE=y`; для `std::thread` — `CONFIG_DYNAMIC_THREAD=y`.
  Без `CONFIG_POSIX_THREADS` C++-приложение, которое тянет guard'ы, локали или `std::mutex`, не слинкуется.
- Пулы POSIX с запасом под libstdc++: +1 рекурсивный мьютекс и +1 condvar для guard'ов статических переменных.
  Пулы инициализируются на `PRE_KERNEL_1`, статические конструкторы C++ — после `POST_KERNEL`,
  так что глобальные `std::mutex` создаются корректно.
- Обходы старого SDK больше не нужны: `--wrap` для `std::chrono::steady_clock::now()` (был `CLOCK_REALTIME`)
  и удаление `-Os` со строки линковки (выбирал multilib без исключений).
- Исключения и блокировки в ISR по-прежнему запрещены.

## Проверка на прошивке

- Release с LTO: в map нет `emergency_pool`, `_GLOBAL__sub_I__ZN9__gnu_cxx9__freeresEv`;
  `eh_globals` в `.tbss`; `__cxa_guard_acquire` вызывает `pthread_once`/`pthread_mutex_lock`.
  Ожидание: потоки +2–3 КБ ROM, `-Os` вместо `-O2` для библиотеки — ещё минус несколько КБ,
  куча +1088 Б свободно.
- На железе: throw/catch в потоках main, workqueue и BT RX без terminate; параллельная инициализация
  function-local static из нескольких потоков; `steady_clock` и `sleep_for` не прыгают после `clock_settime`;
  цикл создания/захвата/уничтожения `std::mutex` не исчерпывает пул.

## При обновлении NCS

1. Перенести `cpp_gthreads` обоих форков на тег sdk-ng, который требует новая NCS.
2. Сверить `stubs/pthread.h` с Zephyr новой NCS.
3. Обновить `ncs_version`, поставить тег, дождаться CI, повторить проверки.

## Риски

- Новое ядро (другой SoC, `CONFIG_FPU=n`) требует добавить его в `t-m4f-m33f` и пересобрать SDK.

## Ссылки

- [zephyr#25569](https://github.com/zephyrproject-rtos/zephyr/issues/25569) — Support for std::thread
- [zephyr#43729](https://github.com/zephyrproject-rtos/zephyr/pull/43729) — std::thread через gthr-posix
- [sdk-ng#735](https://github.com/zephyrproject-rtos/sdk-ng/pull/735), [#751](https://github.com/zephyrproject-rtos/sdk-ng/issues/751), [#753](https://github.com/zephyrproject-rtos/sdk-ng/pull/753), [#774](https://github.com/zephyrproject-rtos/sdk-ng/pull/774) — C11 threads и откат
- [sdk-ng#1142](https://github.com/zephyrproject-rtos/sdk-ng/pull/1142) — `--enable-threads=posix`
- [gcc#30](https://github.com/zephyrproject-rtos/gcc/pull/30) — откат C11 gthreads в форке GCC
