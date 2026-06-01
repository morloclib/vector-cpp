#ifndef __MORLOC__VECTOR_HPP__
#define __MORLOC__VECTOR_HPP__

// C++ implementations of the morloc vector stdlib.
// Vector maps to std::vector<T>; the Nat dimension is phantom at runtime.

#include <cstdint>
#include <stdexcept>
#include <type_traits>
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

// ---------------------------------------------------------------------------
// Typeclass-method backends for @Vector n a@. Vector and List share the
// std::vector runtime form, so the same algorithms apply. Distinct names
// (morloc_vec_*) avoid template-redefinition collisions with root-cpp's
// core.hpp morloc_* family when both are included in pool.cpp.
// ---------------------------------------------------------------------------

template <class T>
inline std::vector<T> morloc_vec_id(const std::vector<T>& xs) { return xs; }

template <class T>
inline std::vector<T> morloc_vec_concat(const std::vector<T>& xs,
                                        const std::vector<T>& ys) {
    std::vector<T> zs;
    zs.reserve(xs.size() + ys.size());
    zs.insert(zs.end(), xs.begin(), xs.end());
    zs.insert(zs.end(), ys.begin(), ys.end());
    return zs;
}

template <class A, class F>
inline auto morloc_vec_map(F f, const std::vector<A>& xs)
    -> std::vector<std::invoke_result_t<F, A>> {
    using B = std::invoke_result_t<F, A>;
    std::vector<B> ys;
    ys.reserve(xs.size());
    for (const auto& x : xs) ys.push_back(f(x));
    return ys;
}

template <class B, class A, class F>
inline B morloc_vec_fold(F&& f, B y, const std::vector<A>& xs) {
    for (const auto& x : xs) y = f(y, x);
    return y;
}

template <class A, class F>
inline A morloc_vec_fold1(F&& f, const std::vector<A>& xs) {
    if (xs.empty()) throw std::runtime_error("fold1: empty vector");
    A acc = xs.front();
    for (size_t i = 1; i < xs.size(); ++i) acc = f(acc, xs[i]);
    return acc;
}

template <class A, class F>
inline std::vector<A> morloc_vec_safeFold1(F&& f, const std::vector<A>& xs) {
    if (xs.empty()) return std::vector<A>();
    A acc = xs.front();
    for (size_t i = 1; i < xs.size(); ++i) acc = f(acc, xs[i]);
    return std::vector<A>{acc};
}

// Structural equality for Vector. std::vector's built-in operator==
// is element-wise and returns a single bool, which is exactly Eq's
// contract. Distinct name keeps the cppmorloc/root-cpp namespace
// clean.
template <class T>
inline bool morloc_vec_eq(const std::vector<T>& xs, const std::vector<T>& ys) {
    return xs == ys;
}

// Lexicographic <= on Vector. std::vector's built-in operator<= already
// implements lexicographic ordering.
template <class T>
inline bool morloc_vec_le(const std::vector<T>& xs, const std::vector<T>& ys) {
    return xs <= ys;
}

#endif
