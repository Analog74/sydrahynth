#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "a5pack.h"
#include "bus.h"
#include "cortex_m3.h"
#include "oled.h"
#include "stm32.h"

namespace hydra {

// The front panel's STM32F103 (high density, 72 MHz) running the "stm32 code"
// firmware section, which is programmed at flash offset 0x10000 (after a
// bootloader that is not part of the firmware pack).
//
// Peripherals (STM32F10x reference manual RM0008 addresses):
//   0x40000000 TIM2-7, RTC, WWDG, IWDG, SPI2/3, USART2/3, UART4/5, I2C1/2,
//              BKP, PWR, DAC (APB1)
//   0x40010000 AFIO, EXTI, GPIOA-E, ADC1-3, TIM1, SPI1, TIM8, USART1 (APB2)
//   0x40020000 DMA1/2, RCC, flash interface, CRC (AHB)
//   0x60000000 FSMC bank 1, 0xA0000000 FSMC registers
// Modelled: RCC, GPIO, AFIO/EXTI, timers, DMA, SPI, USART, ADC, the
// displays on FSMC bank 1. The rest are
// logging stubs.
class PanelMcu {
public:
    static constexpr std::uint64_t kHz = 72'000'000;

    explicit PanelMcu(const FirmwareSection& code);

    void run(std::uint64_t ticks) { core_->run(ticks); }
    CortexM3& core() { return *core_; }
    MmioBus& bus() { return bus_; }
    stm32::Gpio& gpio(int port) { return *gpio_[port]; }
    stm32::Spi& spi(int index) { return *spi_[index]; }
    stm32::Usart& usart1() { return *usart1_; }
    stm32::Usart& uart4() { return *uart4_; }
    stm32::Adc& adc1() { return *adc_; }
    PanelDisplays& displays() { return *displays_; }
    stm32::Dma& dma1() { return *dma1_; }

private:
    template <typename T, typename... Args>
    T& add(std::uint32_t base, Args&&... args);
    void stub(const std::string& name, std::uint32_t base, std::uint32_t size);

    MmioBus bus_;
    std::unique_ptr<CortexM3> core_;
    std::vector<std::unique_ptr<Device>> devices_;
    std::vector<stm32::Timed*> timed_;
    std::array<stm32::Gpio*, 5> gpio_{};
    std::array<stm32::Spi*, 2> spi_{};
    stm32::Usart* usart1_ = nullptr;
    stm32::Usart* uart4_ = nullptr;
    stm32::Adc* adc_ = nullptr;
    PanelDisplays* displays_ = nullptr;
    stm32::Dma* dma1_ = nullptr;
};

}  // namespace hydra
