#include <catch/catch.hpp>

#include <array>
#include <vector>

#include <biscuit/assembler.hpp>

using namespace biscuit;

TEST_CASE("Branch to Self", "[branch]") {
    uint32_t data;
    Assembler as(reinterpret_cast<uint8_t*>(&data), sizeof(data));

    // Simple branch to self with a jump instruction.
    {
        Label label;
        as.Bind(&label);
        as.J(&label);
        REQUIRE(data == 0x0000006F);
    }

    as.RewindBuffer();

    // Simple branch to self with a compressed jump instruction.
    {
        Label label;
        as.Bind(&label);
        as.C_J(&label);
        REQUIRE((data & 0xFFFF) == 0xA001);
    }

    as.RewindBuffer();

    // Simple branch to self with a conditional branch instruction.
    {
        Label label;
        as.Bind(&label);
        as.BNE(x3, x4, &label);
        REQUIRE(data == 0x00419063);
    }

    as.RewindBuffer();

    // Simple branch to self with a compressed branch instruction.
    {
        Label label;
        as.Bind(&label);
        as.C_BNEZ(x15, &label);
        REQUIRE((data & 0xFFFF) == 0xE381);
    }
}

TEST_CASE("Branch with Instructions Between", "[branch]") {
    std::array<uint32_t, 20> data{};
    Assembler as(reinterpret_cast<uint8_t*>(&data), sizeof(data));

    // Simple branch backward
    {
        Label label;
        as.Bind(&label);
        as.ADD(x1, x2, x3);
        as.SUB(x2, x4, x3);
        as.J(&label);
        REQUIRE(data[2] == 0xFF9FF06F);
    }

    as.RewindBuffer();
    data.fill(0);

    // Simple branch forward
    {
        Label label;
        as.J(&label);
        as.ADD(x1, x2, x3);
        as.SUB(x2, x4, x3);
        as.Bind(&label);
        REQUIRE(data[0] == 0x00C0006F);
    }

    as.RewindBuffer();
    data.fill(0);

    // Simple branch backward (compressed)
    {
        Label label;
        as.Bind(&label);
        as.ADD(x1, x2, x3);
        as.SUB(x2, x4, x3);
        as.C_J(&label);
        REQUIRE((data[2] & 0xFFFF) == 0xBFC5);
    }

    as.RewindBuffer();
    data.fill(0);

    // Simple branch forward (compressed)
    {
        Label label;
        as.C_J(&label);
        as.ADD(x1, x2, x3);
        as.SUB(x2, x4, x3);
        as.Bind(&label);
        REQUIRE((data[0] & 0xFFFF) == 0xA0A1);
    }
}

TEST_CASE("Relaxed jumps reserve a fixed slot and reach far labels", "[branch]") {
    constexpr size_t MaxShortJumpNOPCount = (0xFFFFC - 8) / sizeof(uint32_t);
    constexpr size_t FarNOPCount = 0x40000;
    std::vector<uint32_t> data(FarNOPCount + 4);

    const auto DecodeJALImmediate = [](uint32_t instruction) {
        const auto immediate = ((instruction >> 31) << 20) |
                               (((instruction >> 21) & 0x3FF) << 1) |
                               (((instruction >> 20) & 1) << 11) |
                               (((instruction >> 12) & 0xFF) << 12);
        return static_cast<int32_t>(immediate << 11) >> 11;
    };
    const auto DecodeJALRImmediate = [](uint32_t instruction) {
        return static_cast<int32_t>(instruction) >> 20;
    };

    // Near targets use JAL but retain the second instruction as NOP, so a
    // label can be relaxed without changing any later code offsets.
    {
        Assembler as(reinterpret_cast<uint8_t*>(data.data()), data.size() * sizeof(uint32_t));
        Label label;
        as.JRelaxed(x5, &label);
        as.NOP();
        as.Bind(&label);
        REQUIRE(data[0] == 0x00C0006F);
        REQUIRE(data[1] == 0x00000013);
    }
    std::fill(data.begin(), data.end(), 0);

    // The largest aligned positive JAL offset remains in the short form.
    {
        Assembler as(reinterpret_cast<uint8_t*>(data.data()), data.size() * sizeof(uint32_t));
        Label label;
        as.JRelaxed(x5, &label);
        for (size_t i = 0; i < MaxShortJumpNOPCount; ++i) {
            as.NOP();
        }
        as.Bind(&label);
        REQUIRE((data[0] & 0x7F) == 0x6F); // JAL
        REQUIRE(data[1] == 0x00000013);
        REQUIRE(DecodeJALImmediate(data[0]) == 0xFFFFC);
    }
    std::fill(data.begin(), data.end(), 0);

    // Forward labels are initially emitted as a near jump and patched when
    // bound.  The far form must retain the same two-instruction slot.
    {
        Assembler as(reinterpret_cast<uint8_t*>(data.data()), data.size() * sizeof(uint32_t));
        Label label;
        as.JRelaxed(x5, &label);
        for (size_t i = 0; i < FarNOPCount; ++i) {
            as.NOP();
        }
        as.Bind(&label);

        REQUIRE((data[0] & 0x7F) == 0x17); // AUIPC
        REQUIRE(((data[0] >> 7) & 0x1F) == x5.Index());
        REQUIRE((data[1] & 0x7F) == 0x67); // JALR
        REQUIRE(((data[1] >> 7) & 0x1F) == x0.Index());
        REQUIRE(((data[1] >> 15) & 0x1F) == x5.Index());
        const auto high = static_cast<int32_t>(data[0] & 0xFFFFF000);
        const auto target_offset = static_cast<int32_t>(as.GetCursorPointer() - as.GetBufferPointer(0));
        REQUIRE(high + DecodeJALRImmediate(data[1]) == target_offset);
    }

    // A backward out-of-range label uses the same AUIPC/JALR form.
    std::fill(data.begin(), data.end(), 0);
    {
        Assembler as(reinterpret_cast<uint8_t*>(data.data()), data.size() * sizeof(uint32_t));
        Label label;
        as.Bind(&label);
        for (size_t i = 0; i <= FarNOPCount; ++i) {
            as.NOP();
        }
        const auto source_offset = as.GetCursorPointer() - as.GetBufferPointer(0);
        as.JRelaxed(x5, &label);

        const auto source = source_offset / sizeof(uint32_t);
        REQUIRE((data[source] & 0x7F) == 0x17);
        REQUIRE((data[source + 1] & 0x7F) == 0x67);
        const auto high = static_cast<int32_t>(data[source] & 0xFFFFF000);
        REQUIRE(high + DecodeJALRImmediate(data[source + 1]) == -static_cast<int32_t>(source_offset));
    }
}
