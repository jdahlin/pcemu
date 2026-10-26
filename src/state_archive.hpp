#pragma once
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
namespace pc {
class StateArchive;
struct ArmSnapshotAccess {
    template<class T> static void fields(StateArchive &, T &);
};
// Host-only snapshot codec. Runtime objects explicitly enumerate state fields;
// pointers, callbacks, file descriptors and RAM views are never persisted.
// This experimental format is local-build/ABI specific, not a device wire ABI.
class StateArchive {
    std::vector<uint8_t> output;
    std::span<const uint8_t> input;
    size_t position = 0;
public:
    const bool loading;
    StateArchive() : loading(false) {}
    explicit StateArchive(std::span<const uint8_t> data) : input(data), loading(true) {}
    std::vector<uint8_t> take() { return std::move(output); }
    void finish() const {
        if (loading && position != input.size()) throw std::runtime_error("Trailing snapshot state");
    }
    void bytes(void *data, size_t size) {
        if (loading) {
            if (size > input.size() - position) throw std::runtime_error("Truncated snapshot state");
            std::memcpy(data, input.data() + position, size); position += size;
        } else {
            if (size > size_t(512) * 1024 * 1024 - output.size()) throw std::runtime_error("Snapshot metadata exceeds limit");
            const auto *first = static_cast<const uint8_t *>(data);
            output.insert(output.end(), first, first + size);
        }
    }
    template<class... T> void operator()(T &...value) { (field(value), ...); }
    template<class T> void field(T &value) {
        static_assert(!std::is_pointer_v<T>);
        if constexpr (std::is_same_v<T,bool>) {
            uint8_t encoded=value ? 1 : 0; bytes(&encoded,1);
            if (encoded>1) throw std::runtime_error("Invalid snapshot boolean");
            if (loading) value=encoded!=0;
        } else if constexpr (std::is_trivially_copyable_v<T>) bytes(&value, sizeof(value));
        else ArmSnapshotAccess::fields(*this, value);
    }
    template<class C, class Traits, class Alloc> void field(std::basic_string<C,Traits,Alloc> &v) {
        uint64_t n = v.size(); field(n); count(n, sizeof(C));
        if (loading) v.resize(n);
        bytes(v.data(), n * sizeof(C));
    }
    template<class T> void field(std::vector<T> &v) {
        uint64_t n = v.size(); field(n); count(n, sizeof(T));
        if (loading) v.resize(n);
        if constexpr (std::is_trivially_copyable_v<T> && !std::is_same_v<T,bool>) bytes(v.data(), n * sizeof(T));
        else for (size_t i = 0; i < n; ++i) {
            if constexpr (std::is_same_v<T,bool>) { bool x = v[i]; field(x); if (loading) v[i] = x; }
            else field(v[i]);
        }
    }
    template<class T, size_t N> void field(std::array<T,N> &v) { for (auto &x : v) field(x); }
    template<class T> void field(std::deque<T> &v) {
        uint64_t n = v.size(); field(n); count(n, sizeof(T));
        if (loading) v.resize(n);
        for (auto &x : v) field(x);
    }
    template<class K, class V> void field(std::map<K,V> &v) {
        uint64_t n = v.size(); field(n); count(n, sizeof(K) + sizeof(V));
        if (loading) {
            v.clear();
            for (uint64_t i = 0; i < n; ++i) { K key{}; V value{}; (*this)(key,value);
                if (!v.emplace(std::move(key),std::move(value)).second) throw std::runtime_error("Duplicate snapshot key"); }
        } else for (auto &[k,x] : v) { K key = k; (*this)(key,x); }
    }
    template<class T> void field(std::optional<T> &v) {
        bool present = v.has_value(); field(present);
        if (loading) { if (present) v.emplace(); else v.reset(); }
        if (present) field(*v);
    }
    template<class A, class B> void field(std::pair<A,B> &v) { (*this)(v.first,v.second); }
private:
    void count(uint64_t n, size_t element) const {
        if (n > (size_t(512)*1024*1024)/element || (loading && n > input.size()-position))
            throw std::runtime_error("Invalid snapshot collection length");
    }
};
}
