#pragma once

#include <mbgl/util/type_list.hpp>
#include <mbgl/util/indexed_tuple.hpp>
#include <mbgl/util/range.hpp>


namespace mbgl {
namespace style {

/**
 * A statistics entry for a property
 */
template <class Type>
class StatsEntry {
public:
    optional<Type> max;
    bool set;
};

/**
 * Holder for paint property statistics,
 * calculated just before rendering
 *
 * <Ps> the DataDrivenPaintProperties in this collection
 */
template <class... Ps>
class PaintPropertyStatistics {
public:

    using Properties = TypeList<Ps...>;
    using Types = TypeList<StatsEntry<typename Ps::Type>...>;

    template <class TypeList>
    using Tuple = IndexedTuple<Properties, TypeList>;

    class Values : public Tuple<Types> {
    public:
        using Tuple<Types>::Tuple;
    };
    
    template <class P>
    bool has() const {
        return entries.template get<P>().set;
    }

    template <class P>
    auto max() const {
        auto value = entries.template get<P>();
        return *value.max;
    }

    template <class P>
    void add(const float& value) {
        auto& holder = entries.template get<P>();
        holder.max = holder.max ? std::max(*holder.max, value) : value;
        holder.set = true;
    }

    template <class P>
    void add(const Range<float>& value) {
        add<P>(std::max(value.min, value.max));
    }

    template <class P, class T>
    void add(const T&) {
        //NOOP, not interested in these types, yet.
    }

private:

    Values entries;
};

} // namespace style
} // namespace mbgl
