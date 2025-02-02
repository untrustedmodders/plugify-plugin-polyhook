#pragma once

#include <utility>

namespace {
	template<typename T, typename... Rest>
	void hash_combine(std::size_t& seed, const T& v, const Rest&... rest) {
		seed ^= std::hash<T>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		(hash_combine(seed, rest), ...);
	}
}

template<typename T1, typename T2>
struct std::hash<std::pair<T1, T2>> {
	std::size_t operator()(std::pair<T1, T2> const& p) const noexcept {
		std::size_t seed{};
		hash_combine(seed, p.first, p.second);
		return seed;
	}
};
