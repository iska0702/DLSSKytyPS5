// RDNA2 ISA audit sweeps (included by ShaderRecompilerComputeTests.cpp after
// ShaderBdaWriteTests.inc). Each case runs one VOP3 opcode over a table of operand rows and
// compares with the result the AMD RDNA2 ISA reference (section 12.x pseudocode) gives. Run with
// `shader_recompiler_compute_tests --isa-audit-only [name-substring]`: every failing case is
// listed instead of stopping at the first.

namespace IsaAuditTests {

struct Row {
  u32 a = 0, b = 0, c = 0;
  u32 expected = 0;
};

inline u32 Fb(float f) { return std::bit_cast<u32>(f); }
inline float Bf(u32 u) { return std::bit_cast<float>(u); }

constexpr u32 QNaN = 0x7fc00000u;

// Names must outlive the TestCase (it keeps a raw pointer).
inline const char *Keep(std::string s) {
  static std::deque<std::string> names;
  names.push_back(std::move(s));
  return names.back().c_str();
}

// One VOP3 instruction per row: v0..v2 = literals, v3 = result (pre-cleared). NaN results are
// canonicalised in the shader (class test + cndmask) so only NaN-ness is compared.
inline TestCase Sweep(const std::string &name, u32 vop3, ShaderOpcode opcode,
                      const std::vector<Row> &rows, u32 abs = 0, u32 neg = 0,
                      bool clamp = false, u32 omod = 0) {
  using O = ShaderOpcode;
  TestCase test;
  test.name = Keep(name);
  test.compute_info.float_mode = 0xc0;
  test.has_compute_info = true;
  AppendVMovU32(&test.code, 5, 3);
  AppendVMovLiteral(&test.code, 6, QNaN);
  for (u32 i = 0; i < rows.size(); i++) {
    AppendVMovLiteral(&test.code, 0, rows[i].a);
    AppendVMovLiteral(&test.code, 1, rows[i].b);
    AppendVMovLiteral(&test.code, 2, rows[i].c);
    AppendVMovU32(&test.code, 3, 0);
    AppendVop3(&test.code, vop3, 3, Vgpr(0), Vgpr(1), Vgpr(2), abs, 0, clamp, omod, neg);
    test.code.push_back(EncodeVopc(0x88, Vgpr(3), 5));
    test.code.push_back(EncodeVop2(0x01, 4, Vgpr(3), 6));
    AppendStoreVgpr(&test.code, 4, i);
    const bool nan = ((rows[i].expected & 0x7f800000u) == 0x7f800000u) &&
                     (rows[i].expected & 0x7fffffu) != 0u;
    test.expected.push_back(nan ? QNaN : rows[i].expected);
  }
  test.initial.resize(test.expected.size());
  AppendEnd(&test.code);
  test.opcodes = {O::V_MOV_B32, opcode, O::V_CMP_CLASS_F32, O::V_CNDMASK_B32,
                  O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  return test;
}

// ---- reference helpers written from the ISA text ----
inline u32 F32ToF16(u32 bits, bool rtz) {
  const u32 s = (bits >> 16) & 0x8000u;
  const int e = static_cast<int>((bits >> 23) & 0xffu);
  const u32 m = bits & 0x7fffffu;
  if (e == 0xff) return s | (m ? 0x7e00u : 0x7c00u);
  const int ne = e - 112;
  if (ne >= 31) return s | (rtz ? 0x7bffu : 0x7c00u);
  if (ne >= 1) {
    u32 h = (static_cast<u32>(ne) << 10) | (m >> 13);
    const u32 rem = m & 0x1fffu;
    if (!rtz && (rem > 0x1000u || (rem == 0x1000u && (h & 1u)))) h++;
    return s | h;
  }
  const uint64_t mant = m | 0x800000u;
  const int shift = 14 - ne;
  if (shift >= 40) return s;
  uint64_t h = mant >> shift;
  const uint64_t rem = mant & ((1ull << shift) - 1);
  const uint64_t half = 1ull << (shift - 1);
  if (!rtz && (rem > half || (rem == half && (h & 1u)))) h++;
  return s | static_cast<u32>(h);
}

inline u32 Snorm16(u32 bits) {
  float f = Bf(bits);
  if (std::isnan(f)) return 0;
  f = std::min(1.0f, std::max(-1.0f, f));
  return static_cast<u32>(static_cast<int32_t>(std::nearbyint(static_cast<double>(f) * 32767.0))) & 0xffffu;
}

inline u32 Unorm16(u32 bits) {
  float f = Bf(bits);
  if (std::isnan(f)) return 0;
  f = std::min(1.0f, std::max(0.0f, f));
  return static_cast<u32>(std::nearbyint(static_cast<double>(f) * 65535.0)) & 0xffffu;
}

inline u32 Ldexp(u32 a, u32 b) {
  const double r = std::ldexp(static_cast<double>(Bf(a)), static_cast<int>(std::max(-4000, std::min(4000, static_cast<int32_t>(b)))));
  return Fb(static_cast<float>(r));
}

inline int Sext(u32 v, int bits) {
  return static_cast<int>(v << (32 - bits)) >> (32 - bits);
}

inline std::vector<TestCase> MakeCases() {
  using O = ShaderOpcode;
  std::vector<TestCase> cases;
  const float inf = std::numeric_limits<float>::infinity();

  // V_LDEXP_F32 (VOP3 0x362): D = S0 * 2^S1 for any S1; GLSL ldexp is undefined out of range.
  {
    std::vector<Row> rows;
    const std::pair<float, u32> in[] = {
        {1.0f, 10u},          {1.5f, static_cast<u32>(-3)}, {1.0f, 127u},
        {1.0f, 128u},         {1.0f, 200u},                 {1.0f, 0x7fffffffu},
        {1.0f, static_cast<u32>(-200)}, {1.0f, 0x80000000u}, {-3.0f, 2u},
        {0x1p-100f, 150u},    {0.0f, 1000u},                {inf, static_cast<u32>(-5)},
        {0x1p100f, static_cast<u32>(-150)}, {1.0f, 0xffffff00u}};
    for (auto [x, e] : in) rows.push_back({Fb(x), e, 0, Ldexp(Fb(x), e)});
    cases.push_back(Sweep("IsaLdexpF32LargeExponents", 0x362, O::V_LDEXP_F32, rows));
  }
  // V_CVT_PKNORM_I16_F32 / U16: D.lo = (norm)S0, D.hi = (norm)S1, RNE, NaN -> 0, clamped.
  {
    const float v[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 0.25f, inf, -inf,
                       std::numeric_limits<float>::quiet_NaN(), 1e-6f, 0.99999f, 0x1p-16f};
    std::vector<Row> si, un;
    for (size_t i = 0; i < std::size(v); i++) {
      const u32 a = Fb(v[i]), b = Fb(v[(i * 5 + 3) % std::size(v)]);
      si.push_back({a, b, 0, Snorm16(a) | (Snorm16(b) << 16)});
      un.push_back({a, b, 0, Unorm16(a) | (Unorm16(b) << 16)});
    }
    cases.push_back(Sweep("IsaCvtPknormI16F32", 0x368, O::V_CVT_PKNORM_I16_F32, si));
    cases.push_back(Sweep("IsaCvtPknormU16F32", 0x369, O::V_CVT_PKNORM_U16_F32, un));
  }
  // V_CVT_PKRTZ_F16_F32 (VOP3 0x12f): round toward zero, overflow -> max finite.
  {
    const float v[] = {1.0f, -2.5f, 65520.0f, 1e10f, inf, 65504.0f, 0x1p-14f, 0x1.8p-24f, 0x1p-25f, -0x1p-25f, 0.1f, -0.0f};
    std::vector<Row> rows;
    for (size_t i = 0; i < std::size(v); i++) {
      const u32 a = Fb(v[i]), b = Fb(v[(i * 7 + 2) % std::size(v)]);
      rows.push_back({a, b, 0, F32ToF16(a, true) | (F32ToF16(b, true) << 16)});
    }
    cases.push_back(Sweep("IsaCvtPkrtzF16F32", 0x12f, O::V_CVT_PKRTZ_F16_F32, rows));
  }
  // V_CVT_F16_F32 (VOP1 0x0a): round to nearest even; 65520 -> inf.
  {
    const float v[] = {1.0f, 65520.0f, 65519.0f, 0x1p-24f, 0x1p-25f, 0x1.8p-25f, 0.1f, -0.0f, inf, -65504.0f, 0x1p-14f, 0x1.ffcp-15f};
    std::vector<Row> rows;
    for (float f : v) rows.push_back({Fb(f), 0, 0, F32ToF16(Fb(f), false)});
    cases.push_back(Sweep("IsaCvtF16F32", 0x18a, O::V_CVT_F16_F32, rows));
  }
  // Integer multiply family.
  {
    const u32 v[] = {0u, 1u, 0xffffffffu, 0x80000000u, 0x7fffffffu, 0x00ffffffu, 0x01000000u, 0x00800000u, 0xff800000u, 12345u, 0xdeadbeefu};
    std::vector<Row> hu, hi, lo, u24, i24, mu24, mi24;
    for (size_t i = 0; i < std::size(v); i++) {
      const u32 a = v[i], b = v[(i * 3 + 1) % std::size(v)], c = v[(i * 5 + 2) % std::size(v)];
      hu.push_back({a, b, 0, static_cast<u32>((static_cast<uint64_t>(a) * b) >> 32)});
      hi.push_back({a, b, 0, static_cast<u32>(static_cast<uint64_t>((static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b))) >> 32)});
      lo.push_back({a, b, 0, a * b});
      u24.push_back({a, b, 0, (a & 0xffffffu) * (b & 0xffffffu)});
      i24.push_back({a, b, 0, static_cast<u32>(Sext(a & 0xffffffu, 24)) * static_cast<u32>(Sext(b & 0xffffffu, 24))});
      mu24.push_back({a, b, c, (a & 0xffffffu) * (b & 0xffffffu) + c});
      mi24.push_back({a, b, c, static_cast<u32>(Sext(a & 0xffffffu, 24)) * static_cast<u32>(Sext(b & 0xffffffu, 24)) + c});
    }
    cases.push_back(Sweep("IsaMulHiU32", 0x16a, O::V_MUL_HI_U32, hu));
    cases.push_back(Sweep("IsaMulHiI32", 0x16c, O::V_MUL_HI_I32, hi));
    cases.push_back(Sweep("IsaMulLoU32", 0x169, O::V_MUL_LO_U32, lo));
    cases.push_back(Sweep("IsaMulU32U24", 0x10b, O::V_MUL_U32_U24, u24));
    cases.push_back(Sweep("IsaMulI32I24", 0x109, O::V_MUL_I32_I24, i24));
    cases.push_back(Sweep("IsaMadU32U24", 0x143, O::V_MAD_U32_U24, mu24));
    cases.push_back(Sweep("IsaMadI32I24", 0x142, O::V_MAD_I32_I24, mi24));
  }
  // Bit-field family.
  {
    const u32 d[] = {0xdeadbeefu, 0x80000000u, 0xffffffffu, 0x12345678u};
    const std::pair<u32, u32> of[] = {{0, 0}, {0, 8}, {4, 12}, {28, 8}, {31, 31}, {16, 31}, {0xffffffe5u, 0xffffffe9u}, {5, 0x20}, {0, 31}, {1, 1}};
    std::vector<Row> bu, bi, al, ab;
    for (u32 a : d)
      for (auto [o, w] : of) {
        const u32 off = o & 31u, wid = w & 31u;
        const u32 mask = wid ? ((1u << wid) - 1u) : 0u;
        bu.push_back({a, o, w, (a >> off) & mask});
        const int32_t sh = static_cast<int32_t>(a) >> off;
        bi.push_back({a, o, w, wid ? static_cast<u32>(Sext(static_cast<u32>(sh) & mask, static_cast<int>(wid))) : 0u});
        const uint64_t cat = (static_cast<uint64_t>(a) << 32) | 0x89abcdefu;
        al.push_back({a, 0x89abcdefu, o, static_cast<u32>(cat >> (o & 31u))});
        ab.push_back({a, 0x89abcdefu, o, static_cast<u32>(cat >> (8u * (o & 3u)))});
      }
    cases.push_back(Sweep("IsaBfeU32", 0x148, O::V_BFE_U32, bu));
    cases.push_back(Sweep("IsaBfeI32", 0x149, O::V_BFE_I32, bi));
    cases.push_back(Sweep("IsaAlignbitB32", 0x14e, O::V_ALIGNBIT_B32, al));
    cases.push_back(Sweep("IsaAlignbyteB32", 0x14f, O::V_ALIGNBYTE_B32, ab));
    std::vector<Row> bfi;
    for (u32 a : d) for (u32 b : d) bfi.push_back({a, b, 0x0f0f0f0fu, (a & b) | (~a & 0x0f0f0f0fu)});
    cases.push_back(Sweep("IsaBfiB32", 0x14a, O::V_BFI_B32, bfi));
  }
  // FFBH/FFBL/BFREV (VOP3-encoded VOP1).
  {
    const u32 v[] = {0u, 1u, 0x80000000u, 0xffffffffu, 0x00010000u, 0xfffffffeu, 0x7fffffffu, 0xc0000000u, 0x00000100u};
    std::vector<Row> hu, hi, lo, rev;
    for (u32 a : v) {
      u32 r = 0xffffffffu;
      for (int i = 31; i >= 0; i--) if (a >> i & 1u) { r = 31u - i; break; }
      hu.push_back({a, 0, 0, r});
      r = 0xffffffffu;
      if (a != 0u && a != 0xffffffffu) {
        const u32 s = a >> 31;
        for (int i = 30; i >= 0; i--) if ((a >> i & 1u) != s) { r = 31u - i; break; }
      }
      hi.push_back({a, 0, 0, r});
      r = 0xffffffffu;
      for (int i = 0; i < 32; i++) if (a >> i & 1u) { r = i; break; }
      lo.push_back({a, 0, 0, r});
      u32 x = 0;
      for (int i = 0; i < 32; i++) if (a >> i & 1u) x |= 1u << (31 - i);
      rev.push_back({a, 0, 0, x});
    }
    cases.push_back(Sweep("IsaFfbhU32", 0x1b9, O::V_FFBH_U32, hu));
    cases.push_back(Sweep("IsaFfbhI32", 0x1bb, O::V_FFBH_I32, hi));
    cases.push_back(Sweep("IsaFfblB32", 0x1ba, O::V_FFBL_B32, lo));
    cases.push_back(Sweep("IsaBfrevB32", 0x1b8, O::V_BFREV_B32, rev));
  }
  // Int -> float RNE and float -> int saturation.
  {
    const u32 v[] = {0u, 1u, 16777217u, 0xffffffffu, 0x80000000u, 0x7fffffffu, 0xfeffffffu, 33554435u};
    std::vector<Row> fu, fi;
    for (u32 a : v) {
      fu.push_back({a, 0, 0, Fb(static_cast<float>(a))});
      fi.push_back({a, 0, 0, Fb(static_cast<float>(static_cast<int32_t>(a)))});
    }
    cases.push_back(Sweep("IsaCvtF32U32", 0x186, O::V_CVT_F32_U32, fu));
    cases.push_back(Sweep("IsaCvtF32I32", 0x185, O::V_CVT_F32_I32, fi));
  }
  // V_CVT_OFF_F32_I4 table.
  {
    std::vector<Row> rows;
    for (u32 n = 0; n < 16; n++) rows.push_back({n | 0xabcdef00u, 0, 0, Fb(static_cast<float>(Sext(n, 4)) / 16.0f)});
    cases.push_back(Sweep("IsaCvtOffF32I4", 0x18e, O::V_CVT_OFF_F32_I4, rows));
  }
  // Rounding: trunc / ceil / floor / rndne / rpi / flr.
  {
    const float v[] = {0.5f, -0.5f, 1.5f, 2.5f, -2.5f, 3.5f, 0.49999997f, -0.49999997f, 8388609.0f, -8388609.0f, 1e20f, -1e20f, inf, -inf, 0.0f, -0.0f, 123.75f, -123.75f};
    std::vector<Row> tr, ce, fl, rn;
    for (float f : v) {
      tr.push_back({Fb(f), 0, 0, Fb(std::trunc(f))});
      ce.push_back({Fb(f), 0, 0, Fb(std::ceil(f))});
      fl.push_back({Fb(f), 0, 0, Fb(std::floor(f))});
      rn.push_back({Fb(f), 0, 0, Fb(std::nearbyint(f))});
    }
    cases.push_back(Sweep("IsaTruncF32", 0x1a1, O::V_TRUNC_F32, tr));
    cases.push_back(Sweep("IsaCeilF32", 0x1a2, O::V_CEIL_F32, ce));
    cases.push_back(Sweep("IsaFloorF32", 0x1a4, O::V_FLOOR_F32, fl));
    cases.push_back(Sweep("IsaRndneF32", 0x1a3, O::V_RNDNE_F32, rn));
    std::vector<Row> rpi, flr;
    const float w[] = {0.5f, -0.5f, 1.5f, -1.5f, 2.5f, -2.49f, 2147483647.0f, -2147483904.0f, 3e9f, -3e9f, inf, -inf, 123.75f};
    for (float f : w) {
      auto sat = [](double x) {
        if (std::isnan(x)) return 0u;
        if (x >= 2147483648.0) return 0x7fffffffu;
        if (x <= -2147483648.0) return 0x80000000u;
        return static_cast<u32>(static_cast<int32_t>(x));
      };
      rpi.push_back({Fb(f), 0, 0, sat(std::floor(static_cast<double>(f) + 0.5))});
      flr.push_back({Fb(f), 0, 0, sat(std::floor(static_cast<double>(f)))});
    }
    cases.push_back(Sweep("IsaCvtRpiI32F32", 0x18c, O::V_CVT_RPI_I32_F32, rpi));
    cases.push_back(Sweep("IsaCvtFlrI32F32", 0x18d, O::V_CVT_FLR_I32_F32, flr));
  }
  // Transcendental special values and exact points (VOP3-encoded VOP1).
  {
    const auto Mk = [&](std::initializer_list<std::pair<float, float>> l) {
      std::vector<Row> rows;
      for (auto [x, y] : l) rows.push_back({Fb(x), 0, 0, Fb(y)});
      return rows;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    cases.push_back(Sweep("IsaRcpF32Specials", 0x1aa, O::V_RCP_F32,
                          Mk({{0.0f, inf}, {-0.0f, -inf}, {inf, 0.0f}, {-inf, -0.0f}, {2.0f, 0.5f}, {-4.0f, -0.25f}, {nan, nan}})));
    cases.push_back(Sweep("IsaRsqF32Specials", 0x1ae, O::V_RSQ_F32,
                          Mk({{0.0f, inf}, {-0.0f, -inf}, {inf, 0.0f}, {-1.0f, nan}, {4.0f, 0.5f}, {-inf, nan}, {nan, nan}})));
    cases.push_back(Sweep("IsaSqrtF32Specials", 0x1b3, O::V_SQRT_F32,
                          Mk({{0.0f, 0.0f}, {-0.0f, -0.0f}, {inf, inf}, {-1.0f, nan}, {4.0f, 2.0f}, {-inf, nan}, {nan, nan}})));
    cases.push_back(Sweep("IsaLogF32Specials", 0x1a7, O::V_LOG_F32,
                          Mk({{0.0f, -inf}, {-0.0f, -inf}, {inf, inf}, {-1.0f, nan}, {8.0f, 3.0f}, {1.0f, 0.0f}, {0.5f, -1.0f}})));
    cases.push_back(Sweep("IsaExpF32Specials", 0x1a5, O::V_EXP_F32,
                          Mk({{0.0f, 1.0f}, {-inf, 0.0f}, {inf, inf}, {3.0f, 8.0f}, {-1.0f, 0.5f}, {128.0f, inf}, {-200.0f, 0.0f}, {nan, nan}})));
    cases.push_back(Sweep("IsaFractF32", 0x1a0, O::V_FRACT_F32,
                          Mk({{0.25f, 0.25f}, {-0.25f, 0.75f}, {1.5f, 0.5f}, {-1.2f, std::bit_cast<float>(0x3f4ccccc)}, {3.0f, 0.0f}, {-3.0f, 0.0f}, {1e10f, 0.0f}, {inf, nan}})));
  }
  // Cube map family with ties and signs.
  {
    const float c[][3] = {{1, 2, 3}, {-1, 2, 3}, {3, 2, 1}, {2, 3, 1}, {2, -3, 1}, {3, 3, 3}, {-3, 3, 3}, {3, 3, -3},
                          {2, 2, 1}, {-2, 2, 1}, {0, 0, 0}, {3, -2, 3}, {-3, 2, -1}, {-5, 4, 4}};
    std::vector<Row> id, sc, tc, ma;
    for (auto &p : c) {
      const float x = p[0], y = p[1], z = p[2];
      float fid, fsc, ftc, fma;
      if (std::fabs(z) >= std::fabs(x) && std::fabs(z) >= std::fabs(y)) {
        fid = z < 0 ? 5.f : 4.f; fsc = z < 0 ? -x : x; ftc = -y; fma = 2 * z;
      } else if (std::fabs(y) >= std::fabs(x)) {
        fid = y < 0 ? 3.f : 2.f; fsc = x; ftc = y < 0 ? -z : z; fma = 2 * y;
      } else {
        fid = x < 0 ? 1.f : 0.f; fsc = x < 0 ? z : -z; ftc = -y; fma = 2 * x;
      }
      const Row r{Fb(x), Fb(y), Fb(z), 0};
      Row a = r; a.expected = Fb(fid); id.push_back(a);
      a = r; a.expected = Fb(fsc); sc.push_back(a);
      a = r; a.expected = Fb(ftc); tc.push_back(a);
      a = r; a.expected = Fb(fma); ma.push_back(a);
    }
    cases.push_back(Sweep("IsaCubeId", 0x144, O::V_CUBEID_F32, id));
    cases.push_back(Sweep("IsaCubeSc", 0x145, O::V_CUBESC_F32, sc));
    cases.push_back(Sweep("IsaCubeTc", 0x146, O::V_CUBETC_F32, tc));
    cases.push_back(Sweep("IsaCubeMa", 0x147, O::V_CUBEMA_F32, ma));
  }
  // MED3 / MIN3 / MAX3 integer and float.
  {
    const u32 v[] = {0u, 1u, 0x80000000u, 0xffffffffu, 0x7fffffffu, 5u, 0xfffffff0u};
    std::vector<Row> ms, mu;
    for (size_t i = 0; i < std::size(v); i++) {
      const u32 a = v[i], b = v[(i + 2) % std::size(v)], c = v[(i + 5) % std::size(v)];
      int32_t x[3] = {static_cast<int32_t>(a), static_cast<int32_t>(b), static_cast<int32_t>(c)};
      std::sort(x, x + 3);
      ms.push_back({a, b, c, static_cast<u32>(x[1])});
      u32 y[3] = {a, b, c};
      std::sort(y, y + 3);
      mu.push_back({a, b, c, y[1]});
    }
    cases.push_back(Sweep("IsaMed3I32", 0x158, O::V_MED3_I32, ms));
    cases.push_back(Sweep("IsaMed3U32", 0x159, O::V_MED3_U32, mu));
  }
  return cases;
}

inline int Run(VulkanHarness *vulkan, const char *filter) {
  g_soft_fail = true;
  auto cases = MakeCases();
  int failed = 0, ran = 0;
  for (const auto &test : cases) {
    if (filter != nullptr && std::string(test.name).find(filter) == std::string::npos) continue;
    ran++;
    try {
      RunCase(vulkan, test);
    } catch (const SoftFailure &failure) {
      failed++;
      std::printf("[isa-audit] FAIL %s\n", failure.text.c_str());
    }
  }
  std::printf("[isa-audit] %d cases, %d failing\n", ran, failed);
  g_soft_fail = false;
  return failed == 0 ? 0 : 1;
}

} // namespace IsaAuditTests
