#ifndef __MORLOC__VECTOR_HPP__
#define __MORLOC__VECTOR_HPP__

// C++ implementations of the morloc vector stdlib.
// Vector maps to std::vector<T>; the Nat dimension is phantom at runtime.

#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// zeros / ones: T is determined by the *result* type, not by any argument,
// so plain function templates would fail deduction. Each function returns a
// small proxy that holds the construction parameters and exposes a templated
// conversion operator; T is resolved at the call site from the LHS type.
// fill takes the value as its first argument, so T is deducible there and no
// proxy is needed.
// ---------------------------------------------------------------------------

namespace mlc_ctor {

struct Zeros1 {
    int64_t d1;

    template <class T>
    operator std::vector<T>() const {
        return std::vector<T>(static_cast<size_t>(d1), T(0));
    }
};

struct Ones1 {
    int64_t d1;

    template <class T>
    operator std::vector<T>() const {
        return std::vector<T>(static_cast<size_t>(d1), T(1));
    }
};

}  // namespace mlc_ctor

inline mlc_ctor::Zeros1 morloc_zeros1(int64_t d1) {
    return mlc_ctor::Zeros1{d1};
}

inline mlc_ctor::Ones1 morloc_ones1(int64_t d1) {
    return mlc_ctor::Ones1{d1};
}

template <class T>
std::vector<T> morloc_fill1(const T& v, int64_t d1) {
    return std::vector<T>(static_cast<size_t>(d1), v);
}

#endif
