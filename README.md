# Zephyr SDK 1.0.1 с потоками в libstdc++ (`cpp_gthreads`)

Форк [zephyrproject-rtos/sdk-ng](https://github.com/zephyrproject-rtos/sdk-ng) от тега `v1.0.1`.
Описание исходного SDK — в [README upstream](https://github.com/zephyrproject-rtos/sdk-ng/blob/v1.0.1/README.md).
Цель — тулчейн `arm-zephyr-eabi` для Linux x86_64, в котором libstdc++ собрана с моделью потоков `posix`
и TLS, без COW-строк, без аварийного пула исключений и с исключениями в `-Os`-варианте библиотек.

Статус: план, ничего из него ещё не сделано.

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
3. **TLS.** Нужен `_GLIBCXX_HAVE_TLS`, иначе `eh_globals` хранятся в `pthread_key`, а Zephyr `pthread_setspecific`
   в потоках, созданных не через `pthread_create` (main, workqueue, BT RX), возвращает `EINVAL`,
   и libsupc++ вызывает `std::terminate`. Сначала `gcc_cv_have_tls=yes` в окружении CI,
   если не доходит до configure target-библиотек — правка сгенерированного `libstdc++-v3/configure`.

## Изменения в этом репозитории

1. `configs/common.config`, `CT_CC_GCC_EXTRA_CONFIG_ARRAY` += 
   - `--enable-threads=posix`;
   - `--enable-libstdcxx-time=yes` (`steady_clock` через `clock_gettime(CLOCK_MONOTONIC)`, `sleep_for` через `nanosleep`);
   - `--disable-libstdcxx-dual-abi --with-default-libstdcxx-abi=new` (без COW-строк, см. ниже);
   - `--enable-libstdcxx-static-eh-pool --with-libstdcxx-eh-pool-obj-count=0` (без аварийного пула, см. ниже).
2. `stubs/pthread.h` — только объявления, строго по типам Zephyr из той версии NCS, под которую собирается SDK
   (`include/zephyr/posix/posix_types.h`, `pthread.h`): `pthread_mutex_t`, `pthread_cond_t`, `pthread_key_t`,
   `pthread_t` — `uint32_t`; `pthread_once_t` — `struct { bool flag; }`, `PTHREAD_ONCE_INIT {0}`;
   `PTHREAD_MUTEX_INITIALIZER (-1)`, `PTHREAD_COND_INITIALIZER (-1)`, `PTHREAD_MUTEX_RECURSIVE 1`;
   `pthread_attr_t`, `sched_param` как в Zephyr; функции — только реализованные в `lib/posix/options`.
   Заглушка из sdk-ng#1142 не подходит: её `{0}`-инициализаторы Zephyr считает невалидным объектом (`EINVAL`).
   В шапке — версия NCS, из которой заголовок сгенерирован.
3. CI: матрица хост `linux-x86_64` × цель `arm-zephyr-eabi`; шаг копирования `stubs/pthread.h` в
   `picolibc/newlib/libc/include/` и `export gcc_cv_have_tls=yes`. На тег `cpp_gthreads-1.0.1-N` — релиз с полным
   деревом SDK (`sdk_version`, `cmake/`, `gnu/arm-zephyr-eabi`) и файлом-маркером с версией NCS.
   `sdk_version` остаётся `1.0.1`, иначе `find_package(Zephyr-sdk 1.0)` в Zephyr не найдёт SDK.
4. Проверки в CI, сборка падает при несовпадении:
   - `arm-zephyr-eabi-gcc -v` → `Thread model: posix`;
   - `c++config.h` для `thumb/v7e-m+fp/hard` и `thumb/v7e-m+fp/hard/space`: есть `_GLIBCXX_HAS_GTHREADS`,
     `_GLIBCXX_HAVE_TLS`, `_GLIBCXX_USE_CLOCK_MONOTONIC`, `_GLIBCXX_USE_NANOSLEEP`, `_GLIBCXX_USE_DUAL_ABI 0`,
     `_GLIBCXX_GTHREAD_USE_WEAK 0`;
   - в `bits/gthr-default.h` нет `__GTHREAD_MUTEX_INIT`;
   - `functexcept.o` из `space`-варианта `libstdc++.a` вызывает `__cxa_throw`, а не `abort`;
   - в `libstdc++.a` нет `cow-string-inst.o` и символа `emergency_pool`.

## COW-строки

При dual ABI в `std::runtime_error` и других исключениях хранится `__cow_string`, поэтому в прошивку попадают обе
реализации строк: `cow-string-inst.o` (~5,3 КБ) и обычная `string-inst.o` (~5,2 КБ), плюс `cow-stdexcept.o` (~2,4 КБ).
Без dual ABI `__cow_string` — это `basic_string<char>` (`include/std/stdexcept`), COW-объекты не собираются.
Ожидаемая экономия −5…7 КБ ROM. Цена: `sizeof(std::runtime_error)` растёт с 8 до 28 Б.

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
- Пулы POSIX с запасом под libstdc++: +1 рекурсивный мьютекс и +1 condvar для guard'ов статических переменных.
  Пулы инициализируются на `PRE_KERNEL_1`, статические конструкторы C++ — после `POST_KERNEL`,
  так что глобальные `std::mutex` создаются корректно.
- Обходы старого SDK больше не нужны: `--wrap` для `std::chrono::steady_clock::now()` (был `CLOCK_REALTIME`)
  и удаление `-Os` со строки линковки (выбирал multilib без исключений).
- Исключения и блокировки в ISR по-прежнему запрещены.

## Проверка на прошивке

- Release с LTO: в map нет `cow-*.o`, `emergency_pool`, `_GLOBAL__sub_I__ZN9__gnu_cxx9__freeresEv`;
  `eh_globals` в `.tbss`; `__cxa_guard_acquire` вызывает `pthread_once`/`pthread_mutex_lock`.
  Ожидание: потоки +2–3 КБ ROM, COW −5…7 КБ, `-Os` вместо `-O2` для библиотеки — ещё минус несколько КБ,
  куча +1088 Б свободно.
- На железе: throw/catch в потоках main, workqueue и BT RX без terminate; параллельная инициализация
  function-local static из нескольких потоков; `steady_clock` и `sleep_for` не прыгают после `clock_settime`;
  цикл создания/захвата/уничтожения `std::mutex` не исчерпывает пул.

## При обновлении NCS

1. Перенести `cpp_gthreads` обоих форков на тег sdk-ng, который требует новая NCS.
2. Перегенерировать `stubs/pthread.h` из Zephyr новой NCS.
3. Обновить версию NCS в маркере, поставить тег, дождаться CI, повторить проверки.

## Риски

- Сборка всех multilib `rmprofile` в CI — несколько часов; при упоре в лимит сократить до `v7e-m+fp/hard`.
- `--enable-libstdcxx-time=yes` может не пройти link-тесты на bare-metal; тогда `_GLIBCXX_USE_*` задаются
  cache-переменными configure.

## Ссылки

- [zephyr#25569](https://github.com/zephyrproject-rtos/zephyr/issues/25569) — Support for std::thread
- [zephyr#43729](https://github.com/zephyrproject-rtos/zephyr/pull/43729) — std::thread через gthr-posix
- [sdk-ng#735](https://github.com/zephyrproject-rtos/sdk-ng/pull/735), [#751](https://github.com/zephyrproject-rtos/sdk-ng/issues/751), [#753](https://github.com/zephyrproject-rtos/sdk-ng/pull/753), [#774](https://github.com/zephyrproject-rtos/sdk-ng/pull/774) — C11 threads и откат
- [sdk-ng#1142](https://github.com/zephyrproject-rtos/sdk-ng/pull/1142) — `--enable-threads=posix`
- [gcc#30](https://github.com/zephyrproject-rtos/gcc/pull/30) — откат C11 gthreads в форке GCC
