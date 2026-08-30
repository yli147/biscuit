#include <catch/catch.hpp>

#include <biscuit/assembler.hpp>

#include <csetjmp>

namespace {
std::jmp_buf OverflowJump;

void RestartOnOverflow(void* context, size_t required_size) noexcept {
    *static_cast<size_t*>(context) = required_size;
    std::longjmp(OverflowJump, 1);
}
} // namespace

TEST_CASE("Caller-owned CodeBuffer overflow handler", "[code_buffer]") {
    uint8_t buffer[sizeof(uint32_t)] {};
    biscuit::Assembler as {buffer, sizeof(buffer)};
    size_t required_size {};
    as.SetBufferOverflowHandler(RestartOnOverflow, &required_size);

    if (setjmp(OverflowJump) == 0) {
        as.NOP();
        as.NOP();
        FAIL("expected CodeBuffer overflow handler to restart execution");
    }

    REQUIRE(required_size == sizeof(uint32_t));
}
