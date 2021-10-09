#include "abstractops.h"
#include "smt.h"
#include "value.h"
#include <cmath>
#include <map>

using namespace smt;
using namespace std;

static string freshName(string prefix) {
  static int count = 0;
  return prefix + to_string(count ++);
}

// ----- Constants and global vars for abstract floating point operations ------

namespace {
// NaNs, Infs, and +-0 are stored in separate variable
// as they do not work well with map due to comparison issue
optional<Expr> fpconst_zero_pos;
optional<Expr> fpconst_zero_neg;
optional<Expr> fpconst_nan;
optional<Expr> fpconst_inf_pos;
optional<Expr> fpconst_inf_neg;
// Abstract representation of valid fp constants.
map<float, Expr> fpconst_absrepr;
unsigned fpconst_absrepr_num;

const unsigned SIGN_BITS = 1;
const unsigned TYPE_BITS = 1;

// TODO: this must be properly set
// What we need to do is to statically find how many 'different' fp values a
// program may observe.
// FP_BITS must be geq than 1 (otherwise it can't handle reserved values)
unsigned VALUE_BITS = 31;
unsigned FP_BITS;
uint64_t INF_VALUE;
uint64_t NAN_VALUE;
uint64_t SIGNED_VALUE;

void updateConstants() {
  FP_BITS = SIGN_BITS + TYPE_BITS + VALUE_BITS;
  INF_VALUE = 1ull << (uint64_t)VALUE_BITS;
  NAN_VALUE = INF_VALUE + 1;
  SIGNED_VALUE = 1ull << (uint64_t)(TYPE_BITS + VALUE_BITS);

  fpconst_nan = Expr::mkBV(NAN_VALUE, FP_BITS);
  fpconst_inf_pos = Expr::mkBV(INF_VALUE, FP_BITS);
  fpconst_inf_neg = Expr::mkBV(SIGNED_VALUE + INF_VALUE, FP_BITS);
  fpconst_zero_pos = Expr::mkBV(0, FP_BITS);
  fpconst_zero_neg = Expr::mkBV(SIGNED_VALUE + 0, FP_BITS);
}

optional<FnDecl> fp_sumfn;
optional<FnDecl> fp_assoc_sumfn;
optional<FnDecl> fp_dotfn;
optional<FnDecl> fp_addfn;
optional<FnDecl> fp_mulfn;
}

// ----- Constants and global vars for abstract int operations ------

namespace {
map<unsigned, FnDecl> int_sumfn;
map<unsigned, FnDecl> int_dotfn;

FnDecl getIntSumFn(unsigned bitwidth) {
  auto itr = int_sumfn.find(bitwidth);
  if (itr != int_sumfn.end())
    return itr->second;

  FnDecl hashfn(Integer::sort(bitwidth), Integer::sort(bitwidth),
                "int_sum" + to_string(bitwidth));
  int_sumfn.emplace(bitwidth, hashfn);
  return hashfn;
}

FnDecl getIntDotFn(unsigned bitwidth) {
  auto itr = int_dotfn.find(bitwidth);
  if (itr != int_dotfn.end())
    return itr->second;

  FnDecl hashfn(Integer::sort(bitwidth), Integer::sort(bitwidth),
                "int_dot" + to_string(bitwidth));
  int_dotfn.emplace(bitwidth, hashfn);
  return hashfn;
}
}


namespace aop {

static AbsLevelFpDot alFpDot;
static AbsLevelIntDot alIntDot;
static bool isFpAddAssociative;
static UsedAbstractOps usedOps;
static vector<tuple<Expr, Expr, Expr>> staticArrays;
static bool useMultiset;


UsedAbstractOps getUsedAbstractOps() { return usedOps; }

void setAbstraction(
    AbsLevelFpDot afd, AbsLevelIntDot aid, bool addAssoc, unsigned fpBits) {
  alFpDot = afd;
  alIntDot = aid;
  isFpAddAssociative = addAssoc;
  memset(&usedOps, 0, sizeof(usedOps));

  fpconst_absrepr.clear();
  fpconst_absrepr_num = 0;

  assert(fpBits > 0);
  VALUE_BITS = fpBits == 1 ? fpBits : fpBits - 1;
  updateConstants();

  staticArrays.clear();
}

// A set of options that must not change the precision of validation.
void setEncodingOptions(bool use_multiset) {
  useMultiset = use_multiset;
}

bool getFpAddAssociativity() { return isFpAddAssociative; }


Sort fpSort() {
  return Sort::bvSort(FP_BITS);
}

Expr fpConst(float f) {
  if (isnan(f))
    return *fpconst_nan;

  if (isinf(f))
    return signbit(f) ? *fpconst_inf_neg : *fpconst_inf_pos;

  if (f == 0.0f)
    return signbit(f) ? *fpconst_zero_neg : *fpconst_zero_pos;

  // We don't explicitly encode f
  auto itr = fpconst_absrepr.find(f);
  if (itr != fpconst_absrepr.end())
    return itr->second;

  uint64_t absval;
  float abs_f = abs(f);
  if (abs_f == 1.0f) {
    absval = 1;
  } else {
    assert(static_cast<uint64_t>(2 + fpconst_absrepr_num) < INF_VALUE);
    absval = 2 + fpconst_absrepr_num++;
  }

  Expr e_pos = Expr::mkBV(absval, FP_BITS);
  fpconst_absrepr.emplace(abs_f, e_pos);
  Expr e_neg = Expr::mkBV(SIGNED_VALUE + absval, FP_BITS);
  fpconst_absrepr.emplace(-abs_f, e_neg);
  
  return signbit(f) ? e_neg : e_pos;
}

vector<float> fpPossibleConsts(const Expr &e) {
  vector<float> vec;
  for (auto &[k, v]: fpconst_absrepr) {
    if (v.isIdentical(e))
      vec.push_back(k);
  }

  // for 'reserved' values that do not belong to fpconst_absrepr
  if (fpconst_nan && fpconst_nan->isIdentical(e)) {
    vec.push_back(nanf("0"));
  } else if (fpconst_zero_pos && fpconst_zero_pos->isIdentical(e)) {
    vec.push_back(0.0f);
  } else if (fpconst_zero_neg && fpconst_zero_neg->isIdentical(e)) {
    vec.push_back(-0.0f);
  } else if (fpconst_inf_pos && fpconst_inf_pos->isIdentical(e)) {
    vec.push_back(INFINITY);
  } else if (fpconst_inf_neg && fpconst_inf_neg->isIdentical(e)) {
    vec.push_back(-INFINITY);
  }

  return vec;
}

Expr mkZeroElemFromArr(const Expr &arr) {
  unsigned bvsz = arr.select(Index::zero()).sort().bitwidth();
  return Expr::mkBV(0, bvsz);
}


Expr fpAdd(const Expr &f1, const Expr &f2) {
  usedOps.fpAdd = true;
  auto fty = f1.sort();

  if (!fp_addfn) {
    // Fully abstract fp_add(fty, fty) -> fty
    // may be interpreted into 'invalid' value.
    // So fp_add should yield BV[SIGN_BITS + VALUE_BITS].
    // Then, an appropriate value will be inserted to fill in TYPE_BITS 
    auto fp_value_ty = Sort::bvSort(SIGN_BITS + VALUE_BITS);
    fp_addfn.emplace({fty, fty}, fp_value_ty, "fp_add");
  }

  auto fp_zero = Float(0.0f);
  auto fp_id = Float(-0.0f);
  auto fp_inf_pos = Float(INFINITY);
  auto fp_inf_neg = Float(-INFINITY);
  auto fp_nan = Float(nanf("0"));
  auto bv_true = Expr::mkBV(1, 1);
  auto bv_false = Expr::mkBV(0, 1);

  auto fp_add_res = fp_addfn->apply({f1, f2}) + fp_addfn->apply({f2, f1});
  auto fp_add_sign = fp_add_res.getMSB();
  auto fp_add_value = fp_add_res.extract(VALUE_BITS - 1, 0);

  return Expr::mkIte(f1 == fp_id, f2,         // -0.0 + x -> x
    Expr::mkIte(f2 == fp_id, f1,              // x + -0.0 -> x
      Expr::mkIte(f1 == fp_nan, f1,           // NaN + x -> NaN
        Expr::mkIte(f2 == fp_nan, f2,         // x + NaN -> NaN
    // inf + -inf -> NaN, -inf + inf -> NaN
    // IEEE 754-2019 section 7.2 'Invalid operation'
    Expr::mkIte(((f1 == fp_inf_pos) & (f2 == fp_inf_neg)) |
                ((f1 == fp_inf_neg) & (f2 == fp_inf_pos)), fp_nan,
    // inf + x -> inf, -inf + x -> -inf (both commutative)
    // IEEE 754-2019 section 6.1 'Infinity arithmetic'
    Expr::mkIte((f1 == fp_inf_pos) | (f1 == fp_inf_neg), f1,
      Expr::mkIte((f2 == fp_inf_pos) | (f2 == fp_inf_neg), f2,
    // If both operands do not fall into any of the cases above,
    // use fp_add for abstract representation.
    // But fp_add only yields BV[SIGN_BITS + VALUE_BIT], so we must insert
    // type bit(s) into the fp_add result.
    // Fortunately we can just assume that type bit(s) is 0,
    // because the result of fp_add is always some finite value
    // as Infs and NaNs are already handled in the previous Ites.
    // 
    // fp_add will yield some arbitrary sign bit when called.
    // But then there are some cases where we must override this sign bit.
    // If signbit(f1) == 0 and signbit(f2) == 0, signbit(fpAdd(f1, f2)) must be 0.
    // And if signbit(f1) == 1 and signbit(f2) == 1, signbit(fpAdd(f1, f2)) must be 1.
    // But if f1 and f2 have different signs, we can just use an arbitrary sign
    // yielded from fp_add
    Expr::mkIte(((f1.getMSB() == bv_false) & (f2.getMSB() == bv_false)),
      // pos + pos -> pos
      bv_false.concat(fp_add_value.zext(TYPE_BITS)),
    Expr::mkIte(((f1.getMSB() == bv_true) & (f2.getMSB() == bv_true)),
      // neg + neg -> neg
      bv_true.concat(fp_add_value.zext(TYPE_BITS)),
    Expr::mkIte(f1.extract(VALUE_BITS - 1, 0) == f2.extract(VALUE_BITS - 1, 0),
      // x + -x -> 0.0
      fp_zero,
      fp_add_sign.concat(fp_add_value.zext(TYPE_BITS))
  ))))))))));
}

Expr fpMul(const Expr &f1, const Expr &f2) {
  usedOps.fpMul = true;
  // TODO: check that a.get_Sort() == b.get_Sort()
  auto exprSort = f1.sort();

  if (!fp_mulfn)
    fp_mulfn.emplace({exprSort, exprSort}, Float::sort(), "fp_mul");

  auto fp_id = Float(1.0);
  // if neither a nor b is 1.0, the result should be
  // an abstract and pairwise commutative value.
  // therefore we return fp_mul(f1, f2) + fp_mul(f2, f1)
  return Expr::mkIte(f1 == fp_id, f2,                     // if f1 == 1.0, then f2
    Expr::mkIte(f2 == fp_id, f1,                          // elif f2 == 1.0 , then f1
      fp_mulfn->apply({f1, f2}) + fp_mulfn->apply({f2, f1})
          // else fp_mul(f1, f2) + fp_mul(f2, f1)
    )
  );
}

static Expr fpMultisetSum(const Expr &a, const Expr &n) {
  uint64_t length;
  if (!n.isUInt(length))
    assert("Only an array of constant length is supported.");

  auto bag = Expr::mkEmptyBag(Float::sort());
  for (unsigned i = 0; i < length; i ++) {
    bag = bag.insert(a.select(Index(i)));
    bag = bag.simplify();
  }

  if (!fp_assoc_sumfn)
    fp_assoc_sumfn.emplace(bag.sort(), Float::sort(), "fp_assoc_sum");
  Expr result = (*fp_assoc_sumfn)(bag);

  if (n.isNumeral())
    staticArrays.push_back({bag, n, result});

  return result;
}

Expr fpSum(const Expr &a, const Expr &n) {
  usedOps.fpSum = true;
  // TODO: check that a.Sort is Index::Sort() -> Float::Sort()

  if (isFpAddAssociative && useMultiset)
    return fpMultisetSum(a, n);

  if (!fp_sumfn)
    fp_sumfn.emplace(a.sort(), Float::sort(), "fp_sum");
  auto i = Index::var("idx", VarType::BOUND);
  Expr ai = a.select(i);
  Expr zero = mkZeroElemFromArr(a);
  Expr result = (*fp_sumfn)(
      Expr::mkLambda(i, Expr::mkIte(((Expr)i).ult(n), ai, zero)));

  if (isFpAddAssociative && n.isNumeral())
    staticArrays.push_back({a, n, result});

  return result;
}

Expr fpDot(const Expr &a, const Expr &b, const Expr &n) {
  if (alFpDot == AbsLevelFpDot::FULLY_ABS) {
    usedOps.fpDot = true;
    auto i = (Expr)Index::var("idx", VarType::BOUND);
    auto fnSort = a.sort().toFnSort();
    if (!fp_dotfn)
      fp_dotfn.emplace({fnSort, fnSort}, Float::sort(), "fp_dot");

    Expr ai = a.select(i), bi = b.select(i);
    Expr zero = mkZeroElemFromArr(a);
    // Encode commutativity: dot(a, b) = dot(b, a)
    Expr lhs = fp_dotfn->apply({
        Expr::mkLambda(i, Expr::mkIte(i.ult(n), ai, zero)),
        Expr::mkLambda(i, Expr::mkIte(i.ult(n), bi, zero))});
    Expr rhs = fp_dotfn->apply({
        Expr::mkLambda(i, Expr::mkIte(i.ult(n), bi, zero)),
        Expr::mkLambda(i, Expr::mkIte(i.ult(n), ai, zero))});
    return lhs + rhs;

  } else if (alFpDot == AbsLevelFpDot::SUM_MUL) {
    // usedOps.fpMul/fpSum will be updated by the fpMul()/fpSum() calls below
    auto i = (Expr)Index::var("idx", VarType::BOUND);
    Expr ai = a.select(i), bi = b.select(i);
    Expr arr = Expr::mkLambda(i, fpMul(ai, bi));

    return fpSum(arr, n);
  }
  llvm_unreachable("Unknown abstraction level for fp dot");
}

Expr getFpAssociativePrecondition() {
  // Calling this function doesn't make sense if add is not associative
  assert(isFpAddAssociative);

  if (useMultiset) {
    // precondition between `bag equality <-> assoc_sumfn`
    Expr precond = Expr::mkBool(true);
    for (unsigned i = 0; i < staticArrays.size(); i ++) {
      for (unsigned j = i + 1; j < staticArrays.size(); j ++) {
        auto [abag, an, asum] = staticArrays[i];
        auto [bbag, bn, bsum] = staticArrays[j];
        uint64_t alen, blen;
        if (!an.isUInt(alen) || !bn.isUInt(blen) || alen != blen) continue;
        precond = precond & (abag == bbag).implies(asum == bsum);
      }
    }
    precond = precond.simplify();
    return precond;
  }

  // precondition between `hashfn <-> sumfn`
  Expr precond = Expr::mkBool(true);
  for (unsigned i = 0; i < staticArrays.size(); i ++) {
    for (unsigned j = i + 1; j < staticArrays.size(); j ++) {
      auto [a, an, asum] = staticArrays[i];
      auto [b, bn, bsum] = staticArrays[j];
      uint64_t alen, blen;
      if (!an.isUInt(alen) || !bn.isUInt(blen) || alen != blen) continue;
      FnDecl hashfn(Float::sort(), Index::sort(), freshName("fp_hash"));

      auto aVal = hashfn.apply(a.select(Index(0)));
      for (unsigned k = 1; k < alen; k ++)
        aVal = aVal + hashfn.apply(a.select(Index(k)));
      auto bVal = hashfn.apply(b.select(Index(0)));
      for (unsigned k = 1; k < blen; k ++)
        bVal = bVal + hashfn.apply(b.select(Index(k)));

      // precond: sumfn(A) != sumfn(B) -> hashfn(A) != hashfn(B)
      // This means if two summations are different, we can find concrete hash function that hashes into different value.
      auto associativity = (!(asum == bsum)).implies(!(aVal == bVal));
      precond = precond & associativity;
    }
  }
  precond = precond.simplify();
  return precond;
}


// ----- Integer operations ------


Expr intSum(const Expr &a, const Expr &n) {
  usedOps.intSum = true;

  FnDecl sumfn = getIntSumFn(a.sort().bitwidth());
  auto i = Index::var("idx", VarType::BOUND);
  Expr ai = a.select(i);
  Expr zero = mkZeroElemFromArr(a);
  Expr result = sumfn(
      Expr::mkLambda(i, Expr::mkIte(((Expr)i).ult(n), ai, zero)));

  return result;
}

Expr intDot(const Expr &a, const Expr &b, const Expr &n) {
  if (alIntDot == AbsLevelIntDot::FULLY_ABS) {
    usedOps.intDot = true;

    auto i = (Expr)Index::var("idx", VarType::BOUND);
    FnDecl dotfn = getIntDotFn(a.sort().bitwidth());

    Expr ai = a.select(i), bi = b.select(i);
    Expr zero = mkZeroElemFromArr(a);
    Expr lhs = dotfn.apply({
        Expr::mkLambda(i, Expr::mkIte(i.ult(n), ai, zero)),
        Expr::mkLambda(i, Expr::mkIte(i.ult(n), bi, zero))});
    Expr rhs = dotfn.apply({
        Expr::mkLambda(i, Expr::mkIte(i.ult(n), bi, zero)),
        Expr::mkLambda(i, Expr::mkIte(i.ult(n), ai, zero))});
    return lhs + rhs;

  } else if (alIntDot == AbsLevelIntDot::SUM_MUL) {
    auto i = (Expr)Index::var("idx", VarType::BOUND);
    Expr ai = a.select(i), bi = b.select(i);
    Expr arr = Expr::mkLambda(i, ai * bi);

    return intSum(arr, n);
  }
  llvm_unreachable("Unknown abstraction level for int dot");
}

}
