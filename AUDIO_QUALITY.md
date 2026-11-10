# Изменения DSP и проверка звука

## Воспроизведение

- В новом экземпляре Attack = 0 мс вместо 5 мс: начальный транзиент не подавляется огибающей. Пользовательские значения Attack, включая сохранённые проекты, не переопределяются. Для сравнения в старом проекте установите Attack в 0 вручную.
- Существующий обход Stretch при единичной скорости сохранён; расчёт длительности теперь согласован с его допуском. На исходной ноте без изменения длины файл не проходит через FFT.
- При настоящем растяжении Signalsmith использует `presetDefault` с более плотным перекрытием вместо `presetCheaper`; распределение FFT-работы по блокам сохранено.
- High Cut в положении Off остаётся выключенным и при низкой частоте дискретизации хоста.

## Delay и Drive

- Reverse: вместо двух спадов до нуля на каждом стыке — короткое перекрытие обратных фрагментов с косинусным переходом. Изменение длины принимается на границе фрагмента; уходящая головка продолжает читать по прежней длине. Отдельный регрессионный тест проверяет резкую смену Time на непостоянном сигнале.
- Diffusion: четыре коротких all-pass-каскада; ручка изменяет их коэффициент, а не подмешивает фазосдвинутую копию к исходному сигналу. При постоянном значении энергия импульса сохраняется; при 0 обработка обходится.
- Feedback, Diffusion, Bias, Hi-Cut и параметры Drive плавно изменяются за 20 мс.
- Replica: Modern has a brighter, cleaner repeat with a gentle bandwidth limit; Vintage is darker and softly saturated, with the voicing inside the feedback path so it accumulates naturally across repeats. This is a modeled interpretation, not a measured or guaranteed identical copy of the T-Rex Replica pedal; exact matching requires the hardware and level-matched comparison.
- Tape: subtle fixed flutter has been added alongside the existing wow. It has no separate control and only modulates delayed reads.
- Amethyst: its controls are again arranged in one column. Splice is 150–700 ms; grains overlap, are phase-aligned, and are filtered before pitch shifting. This is an original crystalline granular delay, not an exact commercial-device copy.
- Tape and granular Amethyst: four-point interpolation instead of linear interpolation. Tape retains wow and its newly added fixed flutter; changing Time still changes playback speed.
- Saturation in Tape, Replica Vintage and Drive uses first-order ADAA to reduce aliasing. This does not fully eliminate aliasing and is not oversampling; ADAA also slightly changes the high-frequency character.
- Reflections удалена из интерфейса. Старые ID, индекс и диапазон параметра сохранены, но автоматизация и старые значения игнорируются: всегда 1. Amethyst имеет одну генерацию +12 полутонов; Feedback повторяет её без дальнейшего повышения октавы. Старые проекты с несколькими генерациями намеренно звучат иначе.
- Дизайн и размещение остальных элементов интерфейса не изменены.

## Проверка на Windows

Среда: CMake 4.4.3, Visual Studio 18 2026 / MSVC 19.51, JUCE 9.0.5, Release x64.

```text
cmake -S . -B build -DRS_BUILD_TESTS=ON -DRS_COPY_PLUGIN_AFTER_BUILD=OFF
cmake --build build --config Release --target ReverseSamplerTests ReverseSampler_VST3 ReverseSampler_Standalone --parallel 4
build\ReverseSamplerTests_artefacts\Release\ReverseSamplerTests.exe
```

Результат: **282 passed, 0 failed**. Включены реальные рендеры процессора, загрузка WAV, state/undo, UI-переходы, слои и маршрутизация эффектов. Новые проверки находятся в `Tests/AudioQualityTests.h`.

- Погрешность первых 512 сэмплов на исходной ноте: максимум 1.19209e-7; оба канала, Stretch/Resample, forward/reverse. Частота файла равна частоте хоста, длина исходная, FX выключены.
- Diffusion 10/50/100%: энергия импульса амплитудой 0.5 равна 0.25 на двухсекундном рендере.
- Независимость delay от размера блока 64/511 при 44.1/48/96 кГц, плавное изменение Feedback, отсутствие провалов при смене Sync, очистка истории.
- Измеренная алиас-компонента 21 кГц при входе Drive 9 кГц: 0.311168 → 0.186534 относительно прежнего tanh с тем же tone-фильтром. Это один конкретный тест, не универсальная оценка всего спектра.
- Нагрузочный рендер Stretch, 8 нот × 2 слоя, 4 секунды: 622 мс (mono 48k), 1022 мс (stereo 44.1k) в финальном запуске.
- Amethyst, 5 слотов × 2 слоя, блок 128 при 48 кГц: среднее 0.194 мс, p99 0.299 мс, максимум 0.456 мс при бюджете 2.667 мс. Тайминги зависят от машины и нагрузки, это не гарантия real-time при любом проекте.

Снимки нативного JUCE-редактора: `build/quality-snapshots/`.

## Готовые сборки

- Standalone: `build/ReverseSampler_artefacts/Release/Standalone/Reverse Sampler.exe`
- VST3 bundle: `build/ReverseSampler_artefacts/Release/VST3/Reverse Sampler.vst3`

Автокопирование в системную папку VST3 выключено. Для установки используется весь VST3 bundle, не только вложенный бинарный файл.

## Что не подтверждено

Прослушивание на реальном аудиоустройстве и в DAW, субъективное качество diffusion, а также воспроизведение конкретного сэмпла пользователя не проверялись. Автотесты не доказывают отсутствие всех слышимых артефактов при настоящем растяжении/транспонировании. Сборки macOS/AU не выполнялись.


## EQ (0.10.0)
Девять фиксированных секций второго порядка плюс две среза. Matched-z: полюса получаются через impulse invariance, нули подбираются по точной аналоговой АЧХ на 0 Гц, на частоте Найквиста и на центральной частоте. Обработка в double, коэффициенты интерполируются по логарифмической шкале частоты/Q с шагом 32 сэмпла. Нелинейностей нет: Vintage — это форма АЧХ, без насыщения и шума.


## Drive (0.12.0)

Измерено автотестом `Tests/DriveDspTests.cpp` при 44,1 кГц:

- Drive 0 на −40 dBFS: все типы в пределах 0,2 дБ от единицы; громкость по всему диапазону Drive — в пределах 0,12 дБ.
- Гармоники (Drive 50 %): Tube H2 −12 / H3 −23 дБ; Soft Clip H2 −52 / H3 −16 дБ; Tape H3 −17 дБ, чётных нет; Triode H2 −19 / H3 −24 дБ.
- Верх при малом сигнале (15 кГц отн. 1 кГц): Tube −3,9 дБ; Soft Clip −0,2 дБ; Tape −1,4 дБ (щелевые потери); Triode −2,1 дБ (Миллер).
- Tape: head bump +1,5 дБ на 60 Гц; 8 кГц компрессируется на 5,7 дБ сильнее, чем 1 кГц (предыскажения).
- Алиасинг ниже 18 кГц, 2,5 кГц при Drive 100 %: Tube −77, Soft Clip −71, Tape −74, Triode −78 дБ. 5 кГц при Drive 50 %: −107 / −92 / −96 / −80 дБ.
- Горячий вход +12 dBFS при Drive 100 %: выход конечен и ограничен.
- Результат не зависит от размера буфера (побитово), смена типа без щелчка.
- CPU на слот (2 слоя, стерео, 48 кГц): Tube 3,3 %, Soft Clip 2,5 %, Tape 7,4 %, Triode 2,7 % одного ядра.

## Reverb (0.13.0)
Измерено в `Tests/ReverbDspTests.cpp` (g++ -O2, 48 кГц):
- RT60 по EDC совпадает с ручкой Decay: Room/Ambience/Bloom/Shimmer в пределах ±20 %; Plate на коротком Decay длиннее на ~22 % (вклад диффузоров), Terrain на коротком Decay короче на ~31 % (дискретные эхо искажают оценку EDC). RT60 не зависит от частоты дискретизации (96 vs 44.1 кГц: 0.99).
- Уровни wet при настройках по умолчанию выровнены между режимами в пределах ±2 дБ.
- Shimmer: октава +12 полутонов видна в спектре, исходная частота в хвосте подавлена > 30 дБ относительно октавы.
- Bloom: пик огибающей при Attack 0 — 150 мс, при 90 % — 0.93 с; начало хвоста ≈ +33 мс после Pre-Delay.
- Low Cut 400 Гц: 60 Гц в wet ниже на 33 дБ. +12 dBFS на входе — без NaN/Inf. Блок 1 vs 509 сэмплов — бит-в-бит.
- CPU (2 слоя, стерео, 48 кГц, одно ядро): Room 7.5 %, Plate 1.8 %, Ambience 8.4 %, Terrain 7.0 %, Bloom 7.3 %, Shimmer 3.8 %.

## Reverb (0.14.0)

- Новые автотесты: дисперсия Plate (300 Гц позже 6 кГц на 3.1 мс), транзиент Ambience (wet 0–1.5 мс ≈ 0), Terrain темнее Room (6k/1k = 0.14x).
- Bloom attack 1: пик энергии после 300 мс (≈0.5 с).
- CPU, 2 слоя стерео 48 кГц: Room 7.8 %, Plate 2.2 %, Ambience 8.7 %, Terrain 7.7 %, Bloom 7.2 %, Shimmer 3.9 %.

## FX-стойка (0.16.0)

- Переключение: 20 мс затухания старого эффекта (с его ручками) → 20 мс открытия входа нового при сухом выходе → 20 мс перехода выхода к новому. Если открывать вход и выход одновременно, уровень проседает в 1 − w + w² раз (до −2.5 dB на ровном EQ), поэтому шаги идут по очереди.
- Замеры FxRackTests (48 кГц, синус 220 Гц, 0.3); «щелчок» — наибольшая вторая разность сигнала: Tape → Reverb 0.00031 при 0.00025 на ровном звуке; ровный EQ после смены типа 1.00…1.00; Tape 40 → 520 мс 0.00022 при 0.00022; после глайда первое эхо Tape на 520.646 мс, как у слота, всегда стоявшего на 520 мс, разница сигналов 0.
- Те же сценарии в 0.15: выключение слоя A 0.36, Replica → Tape 0.26, Delay → Reverb 0.24, Reverb → Drive 0.20, сброс 0.17.
- Amethyst после глайда приходит к тому же времени и уровню (первое эхо 576.5 мс, −0.0005 dB), но зёрна выровнены иначе, чем у слота, всегда стоявшего на этом Time, поэтому по сэмплам сигнал отличается.
- Головка Tape и Amethyst считается в double: во float одно-полюсное сглаживание 5 мс на задержке 520 мс останавливалось на 0.23 сэмпла раньше цели и не доходило до неё.
- Сброс по запросу: последний звучащий сэмпл — 959-й (20 мс при 48 кГц), наибольший шаг 0.0003.
