# Драйвер управления светодиодами (LED)

Модуль для управления светодиодами через HAL GPIO. Работает в отдельной задаче FreeRTOS (`ledTask`): приложение задаёт режим, переключение пинов выполняется в фоне.

## Состав модуля

| Файл | Назначение |
|------|------------|
| `led.h` / `led.c` | Публичный API и задача `ledTask` |
| `led_config.h` / `led_config.c` | Список светодиодов и привязка к GPIO (порт, пин) |

Зависимости: `main.h`, HAL GPIO, CMSIS-RTOS v2 (`cmsis_os2.h`).

## Архитектура

```
Приложение (main, задачи)
        │  blink_led / set_led / cycle_blink_led / ...
        ▼
   led_ctx[]  ← конфигурация в led_config.c
        │
        ▼
     ledTask  (osDelay(1) → шаг 1 мс)
        │
        ▼
  HAL_GPIO_WritePin / HAL_GPIO_TogglePin
```

Каждый светодиод описывается структурой `led_ctx_t`:

| Поле | Назначение |
|------|------------|
| `led_gpio`, `led_pin` | Аппаратный пин |
| `blink_time` | Счётчик (мс): импульс, полупериод мигания или «включён» |
| `blink_timer` | Полупериод для `cycle_blink_led` (мс) |
| `cycle_flag` | Режим циклического toggle |
| `hard_flag` | Фиксированный уровень, soft-логика не выполняется |

## Режимы работы

### Soft (без `hard_flag`)

| Функция | Поведение |
|---------|-----------|
| `blink_led(id, ms)` | Один импульс: LED включён `ms` мс, затем выключен |
| `cycle_blink_led(id, period_ms)` | Циклическое мигание; `period_ms` — **полный** период (ON + OFF) |
| `set_led(id)` | Постоянно включён (`BLINK_INF`, счётчик не уменьшается) |
| `reset_led(id)` | Выключен (soft) |
| `toggle_led(id)` | Если `blink_time != 0` → `reset_led`, иначе → `set_led` |

### Hard (`hard_flag == 1`)

| Функция | Поведение |
|---------|-----------|
| `hard_set_led(id)` | Принудительно ON (`blink_time != 0`) |
| `hard_reset_led(id)` | Принудительно OFF (`blink_time == 0`) |
| `hard_toggle_led(id)` | Переключение между `hard_set` и `hard_reset` |

Пока `hard_flag == 1`, вызовы soft-функций **не меняют** уровень пина. Чтобы вернуться к миганию или soft-режиму, вызовите `reset_hard_led_flag(id)`.

## Настройка в проекте

### 1. Подключить исходники и заголовки

1. Добавьте каталог с модулем в **Source Location** сборки (чтобы в линковку попали `led.c` и `led_config.c`).
2. Добавьте в **Include paths** каталог с `led.h` / `led_config.h` или родительский каталог для вложенных `#include`.
3. Убедитесь, что оба `.c` файла участвуют в сборке.

### 2. Инициализация GPIO

Пины должны быть сконфигурированы как **выход** до вызова API:

- CubeMX / `MX_GPIO_Init()`;
- BSP платы;
- ручная настройка: тактирование порта, `GPIO_MODE_OUTPUT_PP`.

Порт и пин в `led_config.c` должны совпадать с разводкой.

### 3. Задача FreeRTOS

Создайте задачу **до** `osKernelStart()`:

```c
#include "led.h"

const osThreadAttr_t ledTask_attributes = {
    .name       = "ledTask",
    .stack_size = 128 * 4,
    .priority   = (osPriority_t)osPriorityNormal,
};

osThreadId_t ledTaskHandle = osThreadNew(ledTask, NULL, &ledTask_attributes);
```

Используйте отдельный `osThreadId_t` для `ledTask`.

### 4. Подключение в приложении

```c
#include "led.h"
#include "led_config.h"
```

Пример:

```c
/* Мигание: полный период 500 мс (≈250 мс ON, ≈250 мс OFF) */
cycle_blink_led(LED_0, 500);

/* По событию — жёсткое переключение других LED */
hard_toggle_led(LED_1);
hard_toggle_led(LED_2);

/* Вернуть LED_1 в soft-режим и снова мигать */
reset_hard_led_flag(LED_1);
cycle_blink_led(LED_1, 300);
```

Имена `LED_0`, `LED_1` … задаются в `led_id_t` (`led_config.h`).

### 5. Настройка списка светодиодов (`led_config`)

**Шаг 1.** В `led_config.h` добавьте ID **перед** `END_LED`:

```c
typedef enum {
    LED_0,
    LED_1,
    LED_2,
    LED_N,
    END_LED
} led_id_t;

#define NUM_LEDS  (END_LED)
```

**Шаг 2.** В `led_config.c` заполните `led_ctx[]`:

```c
led_ctx_t led_ctx[NUM_LEDS] = {
    [LED_0] = { .led_gpio = GPIOA, .led_pin = GPIO_PIN_0 },
    [LED_1] = { .led_gpio = GPIOB, .led_pin = GPIO_PIN_1 },
    [LED_2] = { .led_gpio = GPIOC, .led_pin = GPIO_PIN_2 },
    [LED_N] = { .led_gpio = GPIOx, .led_pin = GPIO_PIN_x },
};
```

**Шаг 3.** Настройте GPIO в конфигурации MCU. При использовании макросов BSP/board.h подключите соответствующий заголовок в `led_config.c`.

## API

Все функции принимают `led_id` как `uint8_t` (значения из `led_id_t`). Возвращают `LED_OK` или `UNKNOWN_LED`.

| Функция | Описание |
|---------|----------|
| `blink_led(id, ms)` | Один импульс: ON на `ms` мс, затем OFF. Сбрасывает `cycle_flag`. |
| `cycle_blink_led(id, period_ms)` | Циклическое мигание; `period_ms` — полный период; внутри используется `period_ms / 2` как полупериод между toggle. Рекомендуется `period_ms >= 2`. |
| `set_led(id)` | Постоянно ON (soft). |
| `reset_led(id)` | Постоянно OFF (soft). |
| `hard_set_led(id)` | Принудительно ON; включает `hard_flag`. |
| `hard_reset_led(id)` | Принудительно OFF; включает `hard_flag`. |
| `toggle_led(id)` | Soft-toggle по `blink_time`. |
| `hard_toggle_led(id)` | Hard-toggle по `blink_time`. |
| `reset_hard_led_flag(id)` | Снимает `hard_flag`; нужен перед soft-режимами после `hard_*`. |

### Константы (`led.h`)

| Константа | Значение |
|-----------|----------|
| `BLINK_INF` | `UINT32_MAX` — бесконечное включение в soft-режиме (счётчик не уменьшается) |
| `LED_OK` | Успех |
| `UNKNOWN_LED` | Неверный `led_id` |

## Поведение `ledTask`

- Опрос каждые **1 мс** (`osDelay(1)`); при `configTICK_RATE_HZ = 1000` единицы времени — миллисекунды.
- При старте задачи вызывается `ledTask_init()` — обнуляются флаги и счётчики всех LED.
- **`hard_flag == 1`:** только `WritePin` по `blink_time` (0 → OFF, иначе ON); мигание не выполняется.
- **`cycle_blink_led`:** пока `blink_time > 0`, пин не переключается (только декремент); при `blink_time == 0` — `TogglePin`, затем `blink_time = blink_timer`.
- **`set_led`:** при `blink_time == BLINK_INF` декремент отключён.
- **`blink_led`:** при `cycle_flag == 0` и `blink_time > 0` пин удерживается в SET до обнуления счётчика.

### Циклическое мигание (`cycle_blink_led`)

Параметр `period_ms` — **полный** период одного цикла (условно ON + OFF):

```
period_ms = 500  →  blink_timer = blink_time = 250 мс
                  →  toggle каждые 250 мс
                  →  полный цикл ≈ 500 мс
```

При `period_ms < 2` результат деления `period_ms / 2` равен 0 — мигание становится слишком быстрым (практически каждую 1 мс).

## Переход между режимами

| Из | В | Действие |
|----|---|----------|
| hard | soft (`cycle_blink`, `set_led`, …) | `reset_hard_led_flag(id)`, затем нужная soft-функция |
| soft | hard | `hard_set_led` / `hard_reset_led` |
| мигание | постоянный ON | `set_led` (или `hard_set_led`) |
| мигание | OFF | `reset_led` (или `hard_reset_led`) |

Soft-функции **не сбрасывают** `hard_flag` автоматически.

## Рекомендации

- Не вызывайте `HAL_GPIO_*` для тех же пинов из других задач — возможны гонки с `ledTask`.
- Для потокобезопасности при частых вызовах из нескольких задач рассмотрите mutex вокруг изменений `led_ctx`.
- Не блокируйте `ledTask` длительными операциями.
- После изменения `led_config` выполните полную пересборку проекта.

## Структура каталога

```text
led/
├── README.md
├── led.h
├── led.c
├── led_config.h
└── led_config.c
```
