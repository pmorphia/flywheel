// Flat arrays: an array of structs or numbers stored as one contiguous block instead of a
// vector of slots. Generated code reads and writes the block directly (see flatGet/flatSet in
// src/compiler.cpp) and calls the functions below when it cannot: the array is not flat yet,
// other threads run, or the index is out of range.
//
// An array becomes flat the first time generated code touches an element (flatten), whatever
// created it, and turns back into slots when runtime code that knows nothing about flat storage
// asks for them (Object::slots). The functions here that work on flat arrays directly keep the
// common operations from converting back and forth.
namespace {
struct FlatField {
    // Low byte: the Slot::Kind (Reference for a nested struct); next byte: the width in bits
    // of an integer or real; the rest: the enum id of an enum field.
    int64_t kind;
    int64_t offset;
    const struct FlatLayout *nested;
};
struct FlatLayout {
    int64_t size;       // bytes per element, padding included
    int64_t structure;  // 1: a struct with `count` fields; 0: one number described by fields[0]
    int64_t count;
    const char *classId, *name, *fieldNames; // of the struct's boxed form
    FlatField fields[1];
};
// The offsets generated code uses (Compiler::flatField in src/compiler.cpp).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(offsetof(Object, frozenFlag) == 12 && offsetof(Object, flatData) == 16 &&
              offsetof(Object, flatCount) == 24 && offsetof(Object, flatCapacity) == 32 &&
              offsetof(Object, flatLayout) == 40, "flat array layout");
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
std::mutex &flatMutex() {
    static std::mutex m;
    return m;
}
Slot flatSlot(const FlatField &f, const uint8_t *base);
// A struct in the block as the heap instance the rest of the runtime knows (see boxStruct in
// the compiler). The result holds one reference.
Object *flatBox(const FlatLayout &layout, const uint8_t *base) {
    auto box = new Object(Object::Instance);
    box->classId = layout.classId;
    box->typeName = layout.classId;
    box->text = layout.name;
    box->fieldList = internFields(layout.fieldNames);
    box->leaf = true;
    box->rawSlots.reserve(static_cast<size_t>(layout.count));
    for (int64_t i = 0; i < layout.count; ++i)
        box->rawSlots.push_back(flatSlot(layout.fields[i], base));
    return box;
}
Slot flatSlot(const FlatField &f, const uint8_t *base) {
    Slot s;
    auto at = base + f.offset;
    auto kind = static_cast<Slot::Kind>(f.kind & 0xff);
    auto bits = (f.kind >> 8) & 0xff;
    s.kind = kind;
    switch (kind) {
    case Slot::Reference:
        s.object = flatBox(*f.nested, at);
        break;
    case Slot::Real:
        if (bits == 32) {
            float value;
            std::memcpy(&value, at, 4);
            s.real = value;
        } else
            std::memcpy(&s.real, at, 8);
        break;
    case Slot::Boolean:
        s.integer = *at != 0;
        break;
    case Slot::Unsigned: {
        uint64_t value = 0;
        if (bits == 8)
            value = *at;
        else if (bits == 16) {
            uint16_t v;
            std::memcpy(&v, at, 2);
            value = v;
        } else if (bits == 32) {
            uint32_t v;
            std::memcpy(&v, at, 4);
            value = v;
        } else
            std::memcpy(&value, at, 8);
        s.integer = static_cast<int64_t>(value);
        break;
    }
    default: // Integer, Char, Enum
        if (bits == 8)
            s.integer = static_cast<int8_t>(*at);
        else if (bits == 16) {
            int16_t v;
            std::memcpy(&v, at, 2);
            s.integer = v;
        } else if (bits == 32) {
            int32_t v;
            std::memcpy(&v, at, 4);
            s.integer = v;
        } else
            std::memcpy(&s.integer, at, 8);
        if (kind == Slot::Enum)
            s.real = static_cast<double>(f.kind >> 16);
        break;
    }
    return s;
}
// Writes a slot into the block; false when the slot does not hold what the field expects.
bool flatStore(const FlatField &f, const Slot &s, uint8_t *base) {
    auto at = base + f.offset;
    auto kind = static_cast<Slot::Kind>(f.kind & 0xff);
    auto bits = (f.kind >> 8) & 0xff;
    if (s.kind != kind)
        return false;
    switch (kind) {
    case Slot::Reference: {
        auto box = s.object;
        auto &layout = *f.nested;
        if (!box || box->kind != Object::Instance || box->classId != layout.classId ||
            box->rawSlots.size() != static_cast<size_t>(layout.count))
            return false;
        for (int64_t i = 0; i < layout.count; ++i)
            if (!flatStore(layout.fields[i], box->rawSlots[static_cast<size_t>(i)], at))
                return false;
        return true;
    }
    case Slot::Real:
        if (bits == 32) {
            auto value = static_cast<float>(s.real);
            std::memcpy(at, &value, 4);
        } else
            std::memcpy(at, &s.real, 8);
        return true;
    case Slot::Boolean:
        *at = s.integer != 0;
        return true;
    default:
        if (kind == Slot::Enum && static_cast<int64_t>(s.real) != (f.kind >> 16))
            return false;
        std::memcpy(at, &s.integer, static_cast<size_t>(bits / 8)); // little-endian truncation
        return true;
    }
}
// The element field of an array's layout: the whole struct, or the one number.
FlatField flatElement(const FlatLayout &layout) {
    if (!layout.structure)
        return layout.fields[0];
    return {Slot::Reference, 0, &layout};
}
void flatReserve(Object *p, size_t count) {
    if (count <= p->flatCapacity)
        return;
    auto capacity = std::max<size_t>({count, p->flatCapacity * 2, 8});
    auto size = static_cast<size_t>(p->flatLayout->size);
    auto block = static_cast<uint8_t *>(std::realloc(p->flatData, capacity * size));
    if (!block)
        throw std::bad_alloc();
    p->flatData = block;
    p->flatCapacity = capacity;
}
// Turns an array of slots into a flat block of `layout`. False when it has to stay as it is:
// a slot is empty or holds something else, the race detector is on, or the array has gone
// back and forth too often to be worth converting again.
bool flatten(Object *p, const FlatLayout *layout) {
    if (p->flatLayout)
        return p->flatLayout == layout;
    if (p->kind != Object::Array || p->flatBounces > 8 || raceMode.load(std::memory_order_relaxed))
        return false;
    auto element = flatElement(*layout);
    auto count = p->rawSlots.size();
    auto size = static_cast<size_t>(layout->size);
    uint8_t *block = nullptr;
    auto capacity = std::max<size_t>(count, 8);
    if (count) {
        // An array still being filled (fw_new, then one set per item) fails at its first empty
        // slot, so look at the last one before copying anything.
        auto &last = p->rawSlots.back();
        if (last.kind != static_cast<Slot::Kind>(element.kind & 0xff) ||
            (last.kind == Slot::Reference && !last.object))
            return false;
        block = static_cast<uint8_t *>(std::calloc(capacity, size));
        if (!block)
            throw std::bad_alloc();
        for (size_t i = 0; i < count; ++i)
            if (!flatStore(element, p->rawSlots[i], block + i * size)) {
                std::free(block);
                return false;
            }
    }
    std::vector<Slot> boxes;
    boxes.swap(p->rawSlots);
    p->flatData = block;
    p->flatCount = count;
    p->flatCapacity = block ? capacity : 0;
    p->flatLayout = layout;
    return true; // `boxes` releases the boxed structs
}
// The elements of a flat array as slots, without changing the array.
std::vector<Slot> flatItems(const Object *p) {
    std::vector<Slot> items;
    items.reserve(p->flatCount);
    auto element = flatElement(*p->flatLayout);
    auto size = static_cast<size_t>(p->flatLayout->size);
    for (size_t i = 0; i < p->flatCount; ++i)
        items.push_back(flatSlot(element, p->flatData + i * size));
    return items;
}
uint8_t *flatAt(Object *p, int64_t index) {
    if (index < 0 || static_cast<uint64_t>(index) >= p->flatCount)
        fail("index out of bounds", "IndexError");
    return p->flatData + static_cast<size_t>(index) * static_cast<size_t>(p->flatLayout->size);
}
// A new flat array with the layout of `like` and room for `count` elements.
Object *flatLike(Frame *frame, const Object *like, size_t count) {
    auto result = managed(frame, Object::Array);
    result->flatLayout = like->flatLayout;
    flatReserve(result, count);
    return result;
}
} // namespace
void Object::unflatten() {
    bool shared = activeThreads.load(std::memory_order_acquire) != 0;
    std::unique_lock<std::mutex> lock(flatMutex(), std::defer_lock);
    if (shared)
        lock.lock();
    if (!flatLayout)
        return;
    rawSlots = flatItems(this);
    auto block = flatData;
    flatData = nullptr;
    flatCount = flatCapacity = 0;
    flatLayout = nullptr;
    if (flatBounces < 255)
        ++flatBounces;
    // Another thread may be reading the block through a path that holds no lock on this
    // object; it stays allocated until the object goes.
    if (shared && block)
        more().retired.push_back(block);
    else
        std::free(block);
}
extern "C" {
// Slow paths of generated code. Each returns 1 when it handled the element (or recorded an
// error) and 0 when the array is not flat and cannot become flat, so the caller uses slots.
int64_t fw_flat_read(Object *p, int64_t index, const FlatLayout *layout, void *out) {
    return nativeBoundary([&]() -> int64_t {
        if (!p)
            fail("index out of bounds", "IndexError");
        ObjectGuard guard(p);
        if (!flatten(p, layout))
            return 0;
        std::memset(out, 0, static_cast<size_t>(layout->size));
        std::memcpy(out, flatAt(p, index), static_cast<size_t>(layout->size));
        return 1;
    });
}
int64_t fw_flat_write(Object *p, int64_t index, const FlatLayout *layout, const void *in) {
    if (frozen(p))
        return 1;
    return nativeBoundary([&]() -> int64_t {
        if (!p)
            fail("index out of bounds", "IndexError");
        ObjectGuard guard(p);
        if (!flatten(p, layout))
            return 0;
        std::memcpy(flatAt(p, index), in, static_cast<size_t>(layout->size));
        return 1;
    });
}
int64_t fw_flat_push(Object *p, const FlatLayout *layout, const void *in) {
    if (frozen(p))
        return 1;
    return nativeBoundary([&]() -> int64_t {
        if (!p)
            fail("push to null", "ValueError");
        ObjectGuard guard(p);
        if (!flatten(p, layout))
            return 0;
        flatReserve(p, p->flatCount + 1);
        auto size = static_cast<size_t>(layout->size);
        std::memcpy(p->flatData + p->flatCount * size, in, size);
        ++p->flatCount;
        return 1;
    });
}
int64_t fw_flat_pop(Object *p, const FlatLayout *layout, void *out) {
    if (frozen(p))
        return 1;
    return nativeBoundary([&]() -> int64_t {
        if (!p)
            fail("pop from empty array", "IndexError");
        ObjectGuard guard(p);
        if (!flatten(p, layout))
            return 0;
        std::memset(out, 0, static_cast<size_t>(layout->size));
        if (!p->flatCount)
            fail("pop from empty array", "IndexError");
        --p->flatCount;
        auto size = static_cast<size_t>(layout->size);
        std::memcpy(out, p->flatData + p->flatCount * size, size);
        return 1;
    });
}
// The address and length of an array's block, for passing it to native code without copying.
// The array is made flat first; null when it cannot be (the caller copies instead).
void *fw_flat_data(Object *p, const FlatLayout *layout, int64_t *count) {
    return nativeBoundary([&]() -> void * {
        *count = 0;
        if (!p)
            return nullptr;
        ObjectGuard guard(p);
        if (!flatten(p, layout))
            return nullptr;
        flatReserve(p, 1); // an empty array still has an address
        *count = static_cast<int64_t>(p->flatCount);
        return p->flatData;
    });
}
}
