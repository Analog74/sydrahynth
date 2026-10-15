#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "bus.h"
#include "cortex_m3.h"

// STM32F10x peripherals (register layouts per RM0008), modelled at the level the
// panel firmware needs. Time is the core's cycle count (72 MHz).
namespace hydra::stm32 {

// Independent watchdog: never bites; the firmware reloads it constantly.
class Iwdg : public Device {
public:
    std::string_view name() const override { return "iwdg"; }
    std::uint32_t read(std::uint32_t, int) override { return 0; }
    void write(std::uint32_t, std::uint32_t, int) override {}
};

// Something that needs to run at a point in time. Its next event changes only
// through advance() or a register write, which sets *rescheduled (the board
// caches the earliest event).
class Timed {
public:
    virtual ~Timed() = default;
    virtual std::uint64_t nextEvent() const = 0;  // UINT64_MAX if none
    virtual void advance(std::uint64_t now) = 0;
    bool* rescheduled = nullptr;

protected:
    void reschedule() {
        if (rescheduled)
            *rescheduled = true;
    }
};

// Reset and clock control: oscillators and the PLL are ready as soon as they
// are switched on, and the selected system clock is active at once.
class Rcc : public Device {
public:
    std::string_view name() const override { return "rcc"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

private:
    std::array<std::uint32_t, 11> regs_{0x00000083};  // CR resets with HSION and trim
};

// One GPIO port. Pin levels follow the port configuration: outputs drive ODR
// (alternate-function push-pull outputs read high), open-drain outputs are high
// unless driven low by ODR or externally; inputs read the external level if one
// is driven, otherwise their pull (CNF 10: ODR selects up/down) or 0 if floating.
class Gpio : public Device {
public:
    Gpio(std::string name) : name_(std::move(name)) {}
    std::string_view name() const override { return name_; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    std::uint16_t levels() const;
    std::uint16_t output() const { return odr_; }
    // Drive pins from outside the chip (buttons, other chips).
    void drive(std::uint16_t mask, bool high);
    void release(std::uint16_t mask);

    // Called with the old and new pin levels whenever they change.
    std::vector<std::function<void(std::uint16_t before, std::uint16_t after)>> onChange;
    // Called with the old and new output register (ODR) whenever it changes,
    // regardless of pin configuration (board signals sampled from ODR).
    std::function<void(std::uint16_t before, std::uint16_t after)> onOutputChange;

private:
    void update(std::uint16_t before);

    std::string name_;
    std::uint32_t crl_ = 0x44444444, crh_ = 0x44444444;  // floating inputs
    std::uint16_t odr_ = 0, driven_ = 0, drivenLevels_ = 0;
    std::uint32_t lckr_ = 0;
};

// External interrupt controller; AFIO selects which port feeds each line.
class Exti : public Device {
public:
    explicit Exti(CortexM3& core) : core_(core) {}
    std::string_view name() const override { return "exti"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    void pinChanged(int port, int pin, bool level);
    std::array<std::uint32_t, 4> lineSource{};  // AFIO_EXTICR1-4

private:
    void raise(std::uint32_t lines);

    CortexM3& core_;
    std::uint32_t imr_ = 0, emr_ = 0, rtsr_ = 0, ftsr_ = 0, pr_ = 0;
};

class Afio : public Device {
public:
    explicit Afio(Exti& exti) : exti_(exti) {}
    std::string_view name() const override { return "afio"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

private:
    Exti& exti_;
    std::uint32_t evcr_ = 0, mapr_ = 0, mapr2_ = 0;
};

// General-purpose/advanced timer: the counter is derived from time
// (prescaler, auto-reload); update events set UIF and, with UIE, interrupt.
class Timer : public Device, public Timed {
public:
    Timer(std::string name, CortexM3& core, int irq) : name_(std::move(name)), core_(core), irq_(irq) {}

    std::string_view name() const override { return name_; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;
    std::uint64_t nextEvent() const override;
    void advance(std::uint64_t now) override;

private:
    std::uint64_t period() const { return std::uint64_t(psc_ + 1) * (arr_ + 1); }
    std::uint32_t counterAt(std::uint64_t now) const;

    std::string name_;
    CortexM3& core_;
    int irq_;
    std::uint32_t cr1_ = 0, dier_ = 0, sr_ = 0, psc_ = 0, arr_ = 0xFFFF, stoppedCount_ = 0;
    std::uint64_t start_ = 0;  // time of the last counter wrap
    std::array<std::uint32_t, 20> other_{};
};

// DMA controller. A transfer takes `cyclesPerItem` per item and completes as
// a whole at the end: all items are moved, flags and interrupts are raised,
// circular channels restart.
class Dma : public Device, public Timed {
public:
    Dma(std::string name, CortexM3& core, std::vector<int> channelIrqs)
        : name_(std::move(name)), core_(core), irqs_(std::move(channelIrqs)), channels_(irqs_.size()) {}
    std::string_view name() const override { return name_; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;
    std::uint64_t nextEvent() const override;
    void advance(std::uint64_t now) override;

    std::uint64_t cyclesPerItem = 90;
    // Per-channel override of cyclesPerItem (0: use cyclesPerItem).
    std::array<std::uint64_t, 8> channelCyclesPerItem{};
    std::uint64_t cyclesFor(std::size_t channel) const {
        return channel < channelCyclesPerItem.size() && channelCyclesPerItem[channel] ? channelCyclesPerItem[channel] : cyclesPerItem;
    }
    // Diagnostic (channel 1): completions while its handler runs, and while its
    // previous completion was still unacknowledged (TCIF set).
    std::uint64_t completionsInHandler = 0, completionsUnacknowledged = 0;
    // Peripherals that pull their memory-to-peripheral data item by item (e.g. an
    // SPI slave's TX stream); channels feeding them are not completed by time.
    std::vector<std::uint32_t> pacedPeripherals;
    // A channel whose completion waits while its own interrupt handler runs
    // (-1: none); see PanelMcu.
    int holdWhileHandler = -1;
    // Next item for a paced peripheral, or nullopt if no channel is feeding it.
    std::optional<std::uint32_t> pull(std::uint32_t peripheral);
    // Whether an enabled memory-to-peripheral channel still has items for `peripheral`.
    bool feeding(std::uint32_t peripheral) const;
    // Called when ISR is read repeatedly, unchanged, in quick succession: the CPU
    // is waiting for a transfer to complete.
    std::function<void()> onIdlePoll;
    // Observes every completed memory-to-peripheral transfer: (peripheral address, items).
    std::function<void(std::uint32_t peripheral, const std::vector<std::uint32_t>& items)> onTransfer;

private:
    struct Channel {
        std::uint32_t ccr = 0, cndtr = 0, cpar = 0, cmar = 0;
        std::uint32_t remaining = 0;
        std::uint64_t completesAt = UINT64_MAX;
    };
    void complete(int channel);
    static constexpr std::uint64_t kHoldRetry = 64;

    std::string name_;
    CortexM3& core_;
    std::vector<int> irqs_;
    std::vector<Channel> channels_;
    std::uint32_t isr_ = 0;
    std::uint32_t polledIsr_ = 0;
    std::uint64_t polledAt_ = 0;
    int polls_ = 0;
};

// SPI. As a slave, the master calls exchange() for each byte: the slave
// returns the byte in DR (if loaded) and receives the master's byte.
class Spi : public Device {
public:
    Spi(std::string name, CortexM3& core, int irq) : name_(std::move(name)), core_(core), irq_(irq) {}
    std::string_view name() const override { return name_; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    std::uint8_t exchange(std::uint8_t fromMaster);
    bool enabled() const { return cr1_ & 0x40; }
    // Master mode: called for each byte written to DR; returns the byte received.
    std::function<std::uint8_t(std::uint8_t)> onMasterTransfer;
    // Slave TX DMA (CR2.TXDMAEN): supplies the next byte to send.
    std::function<std::optional<std::uint32_t>()> pullTx;
    bool rxPending() const { return sr_ & 1; }
    std::uint64_t overruns = 0;  // diagnostic: bytes received before the previous one was read
    // Called when the received byte is read (RXNE set). Reading DR again
    // afterwards has no side effects.
    std::function<void()> onDataRead;

private:
    void updateIrq();

    std::string name_;
    CortexM3& core_;
    int irq_;
    std::uint32_t cr1_ = 0, cr2_ = 0, sr_ = 2, rx_ = 0;
    std::uint8_t tx_ = 0xFF;
    bool txLoaded_ = false;
};

class Usart : public Device {
public:
    Usart(std::string name, CortexM3& core, int irq) : name_(std::move(name)), core_(core), irq_(irq) {}
    std::string_view name() const override { return name_; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    void receive(std::uint8_t byte);
    std::function<void(std::uint8_t)> onTransmit;

private:
    void updateIrq();

    std::string name_;
    CortexM3& core_;
    int irq_;
    std::deque<std::uint8_t> rx_;
    std::uint32_t sr_ = 0xC0, brr_ = 0, cr1_ = 0, cr2_ = 0, cr3_ = 0, gtpr_ = 0;
};

// ADC (regular group only). Calibration finishes at once; a start sets EOC and
// reading DR returns the next channel of the sequence (SQR1-3), clearing EOC
// after the last one unless continuous. Channel values come from `sample`.
class Adc : public Device {
public:
    Adc(std::string name, CortexM3& core, int irq) : name_(std::move(name)), core_(core), irq_(irq) {}
    std::string_view name() const override { return name_; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    std::function<std::uint16_t(int channel)> sample;

private:
    int sequenceChannel(int index) const;

    std::string name_;
    CortexM3& core_;
    int irq_;
    std::uint32_t sr_ = 0, cr1_ = 0, cr2_ = 0;
    std::array<std::uint32_t, 15> other_{};  // SMPR1/2, JOFR1-4, HTR, LTR, SQR1-3, JSQR, ...
    int index_ = 0;
};

}  // namespace hydra::stm32
