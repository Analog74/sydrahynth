#include "panel_mcu.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>

namespace hydra {

template <typename T, typename... Args>
T& PanelMcu::add(std::uint32_t base, Args&&... args) {
    auto device = std::make_unique<T>(std::forward<Args>(args)...);
    T& ref = *device;
    bus_.map(base, 0x400, device.get());
    if constexpr (std::is_base_of_v<stm32::Timed, T>) {
        timed_.push_back(&ref);
        ref.rescheduled = &core_->eventsChanged;
    }
    devices_.push_back(std::move(device));
    return ref;
}

PanelMcu::PanelMcu(const FirmwareSection& code) {
    core_ = std::make_unique<CortexM3>(bus_);
    core_->loadFlash(0x10000, code.data);
    CortexM3& core = *core_;

    add<stm32::Rcc>(0x40021000);
    auto& exti = add<stm32::Exti>(0x40010400, core);
    add<stm32::Afio>(0x40010000, exti);
    for (int port = 0; port < 5; ++port) {
        auto& gpio = add<stm32::Gpio>(0x40010800 + 0x400 * port, std::string("gpio") + static_cast<char>('a' + port));
        gpio.onChange.push_back([&exti, port](std::uint16_t before, std::uint16_t after) {
            for (int pin = 0; pin < 16; ++pin)
                if ((before ^ after) >> pin & 1)
                    exti.pinChanged(port, pin, after >> pin & 1);
        });
        gpio_[port] = &gpio;
    }

    const struct {
        const char* name;
        std::uint32_t base;
        int irq;
    } timers[] = {{"tim1", 0x40012C00, 25}, {"tim2", 0x40000000, 28}, {"tim3", 0x40000400, 29},
                  {"tim4", 0x40000800, 30}, {"tim5", 0x40000C00, 50}, {"tim6", 0x40001000, 54},
                  {"tim7", 0x40001400, 55}, {"tim8", 0x40013400, 44}};
    for (const auto& t : timers)
        add<stm32::Timer>(t.base, t.name, core, t.irq);
    auto& dma1 = add<stm32::Dma>(0x40020000, "dma1", core, std::vector<int>{11, 12, 13, 14, 15, 16, 17});
    // The ADC scan's DMA channel (1) completes while its handler runs only if
    // the handler is slow; the handler filters the buffer and then steps the
    // multiplexer, so a buffer converted before that step would be filed under
    // the next position (the encoders' values then jump). On the hardware TIM2,
    // which triggers the conversions, is stopped meanwhile; here the completion
    // waits until the handler has returned. HYDRA_NO_ADC_HOLD=1 switches it off.
    if (!std::getenv("HYDRA_NO_ADC_HOLD"))
        dma1.holdWhileHandler = 0;
    // The scan's pace: 180 cycles per conversion (a full pass of 8 x 50 in 1 ms;
    // the hardware takes 16 ms, the reference 0.5 ms). Most of the panel's work
    // is filtering these buffers, so this halves it; slower paces gain nothing
    // more. HYDRA_ADC_DMA_CYCLES overrides it.
    dma1.channelCyclesPerItem[0] = 180;
    if (const char* adcCycles = std::getenv("HYDRA_ADC_DMA_CYCLES"))
        dma1.channelCyclesPerItem[0] = std::strtoull(adcCycles, nullptr, 10);
    // The WS2812 LED chains' DMA (channels 2, 3, 4, 6): the main loop refreshes
    // all chains after each frame from the main CPU and only then reports ready
    // for the next. At the LEDs' real pace (90 cycles per item, 800 kHz) that
    // takes 2 ms, which limits the link to ~340 frames per second; the main
    // CPU's display task offers ~1400 into a one-frame buffer, so which frames
    // get through beats with its timing, and with some patches the waveform's
    // screen frame came through only 10-20 times a second. At 60 cycles the
    // waveform redraws 42-81 times per second over 16 patches (main thread
    // ~6 % busier). The LED colours are unaffected. HYDRA_LED_DMA_CYCLES
    // overrides it (90: the hardware's pace).
    std::uint64_t ledCycles = 60;
    if (const char* overridden = std::getenv("HYDRA_LED_DMA_CYCLES"))
        ledCycles = std::strtoull(overridden, nullptr, 10);
    for (const std::size_t ch : {1u, 2u, 3u, 5u})
        dma1.channelCyclesPerItem[ch] = ledCycles;
    dma1.pacedPeripherals = {0x4000380C, 0x4001300C};  // SPI2_DR, SPI1_DR
    dma1_ = &dma1;
    if (!std::getenv("HYDRA_NO_POLL_IDLE"))
        dma1.onIdlePoll = [&core] { core.idleUntilEvent(); };
    add<stm32::Dma>(0x40020400, "dma2", core, std::vector<int>{56, 57, 58, 59, 59});
    spi_[0] = &add<stm32::Spi>(0x40013000, "spi1", core, 35);
    spi_[1] = &add<stm32::Spi>(0x40003800, "spi2", core, 36);
    spi_[0]->pullTx = [&dma1] { return dma1.pull(0x4001300C); };
    spi_[1]->pullTx = [&dma1] { return dma1.pull(0x4000380C); };
    add<stm32::Spi>(0x40003C00, "spi3", core, 51);
    usart1_ = &add<stm32::Usart>(0x40013800, "usart1", core, 37);
    add<stm32::Usart>(0x40004400, "usart2", core, 38);
    add<stm32::Usart>(0x40004800, "usart3", core, 39);
    adc_ = &add<stm32::Adc>(0x40012400, "adc1", core, 18);
    add<stm32::Adc>(0x40012800, "adc2", core, 18);
    uart4_ = &add<stm32::Usart>(0x40004C00, "uart4", core, 52);
    add<stm32::Usart>(0x40005000, "uart5", core, 53);

    const struct {
        const char* name;
        std::uint32_t base;
    } stubs[] = {
        {"rtc", 0x40002800},  {"wwdg", 0x40002C00}, {"i2c1", 0x40005400},
        {"i2c2", 0x40005800}, {"bkp", 0x40006C00},  {"pwr", 0x40007000},  {"dac", 0x40007400},
        {"adc3", 0x40013C00}, {"flashif", 0x40022000},
        {"crc", 0x40023000},
    };
    for (const auto& s : stubs)
        stub(s.name, s.base, 0x400);
    // The independent watchdog is reloaded in every main loop iteration (over
    // 100000 times a second): a device without logging.
    bus_.map(0x40003000, 0x400, devices_.emplace_back(std::make_unique<stm32::Iwdg>()).get());
    displays_ = static_cast<PanelDisplays*>(devices_.emplace_back(std::make_unique<PanelDisplays>()).get());
    bus_.map(0x60000000, 0x10000000, displays_);
    stub("fsmc", 0xA0000000, 0x1000);

    core.nextEvent = [this](std::uint64_t) {
        std::uint64_t next = UINT64_MAX;
        for (const stm32::Timed* t : timed_)
            next = std::min(next, t->nextEvent());
        return next;
    };
    core.advance = [this](std::uint64_t now) {
        for (stm32::Timed* t : timed_)
            t->advance(now);
    };
    gpio_[0]->drive(1u << 9, true);  // PA9 is held high by the board
    core.reset(CortexM3::kFlashBase + 0x10000);
}

void PanelMcu::stub(const std::string& name, std::uint32_t base, std::uint32_t size) {
    auto& device = devices_.emplace_back(std::make_unique<StubDevice>(name, bus_.log(), base));
    bus_.map(base, size, device.get());
}

}  // namespace hydra
