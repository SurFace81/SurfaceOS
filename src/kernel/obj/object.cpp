// Kernel objects and handle tables. See obj/object.h.

#include "../../include/obj/object.h"
#include "../../sdk/include/abi/errno.h"

namespace kobj
{
    void init(kobject* o, const kobject_ops* ops)
    {
        o->ops = ops;
        o->refcnt = 1;
    }

    void get(kobject* o)
    {
        o->refcnt++;
    }

    void put(kobject* o)
    {
        if (o->refcnt == 0)
            return;                 // already gone: a double put, not a crash
        if (--o->refcnt == 0 && o->ops->destroy)
            o->ops->destroy(o);
    }

    obj_type type(const kobject* o)
    {
        return o->ops->type;
    }
}

namespace handles
{
    namespace
    {
        inline bool in_range(sint32_t h)
        {
            return h >= 0 && h < HANDLE_TABLE_SIZE;
        }

        void release(handle_slot* s)
        {
            kobject* o = s->obj;
            s->obj = nullptr;
            s->flags = 0;
            if (o)
                kobj::put(o);
        }
    }

    void init(handle_table* t)
    {
        for (sint32_t i = 0; i < HANDLE_TABLE_SIZE; i++)
        {
            t->slots[i].obj = nullptr;
            t->slots[i].flags = 0;
        }
    }

    sint32_t lowest_free(const handle_table* t, sint32_t min)
    {
        if (min < 0)
            min = 0;
        for (sint32_t i = min; i < HANDLE_TABLE_SIZE; i++)
            if (!t->slots[i].obj)
                return i;
        return -1;
    }

    sint64_t install(handle_table* t, kobject* o, uint32_t flags, sint32_t min,
                     sint32_t* out)
    {
        if (min < 0 || min >= HANDLE_TABLE_SIZE)
            return -EINVAL;
        sint32_t h = lowest_free(t, min);
        if (h < 0)
            return -EMFILE;

        kobj::get(o);
        t->slots[h].obj = o;
        t->slots[h].flags = flags;
        *out = h;
        return 0;
    }

    sint64_t install_at(handle_table* t, kobject* o, uint32_t flags, sint32_t h)
    {
        if (!in_range(h))
            return -EBADF;

        // Reference first: `o` may be the very object the slot holds now.
        kobj::get(o);
        release(&t->slots[h]);
        t->slots[h].obj = o;
        t->slots[h].flags = flags;
        return 0;
    }

    kobject* get(const handle_table* t, sint32_t h, obj_type want, sint64_t* rc)
    {
        kobject* o = in_range(h) ? t->slots[h].obj : nullptr;
        if (!o || (want != obj_type::None && kobj::type(o) != want))
        {
            *rc = -EBADF;
            return nullptr;
        }
        *rc = 0;
        return o;
    }

    sint64_t close(handle_table* t, sint32_t h)
    {
        if (!in_range(h) || !t->slots[h].obj)
            return -EBADF;
        release(&t->slots[h]);
        return 0;
    }

    sint64_t get_flags(const handle_table* t, sint32_t h)
    {
        if (!in_range(h) || !t->slots[h].obj)
            return -EBADF;
        return (sint64_t)t->slots[h].flags;
    }

    sint64_t set_flags(handle_table* t, sint32_t h, uint32_t flags)
    {
        if (!in_range(h) || !t->slots[h].obj)
            return -EBADF;
        t->slots[h].flags = flags;
        return 0;
    }

    void fork(handle_table* dst, const handle_table* src)
    {
        for (sint32_t i = 0; i < HANDLE_TABLE_SIZE; i++)
        {
            dst->slots[i] = src->slots[i];
            if (dst->slots[i].obj)
                kobj::get(dst->slots[i].obj);
        }
    }

    void close_flagged(handle_table* t, uint32_t flag)
    {
        for (sint32_t i = 0; i < HANDLE_TABLE_SIZE; i++)
            if (t->slots[i].obj && (t->slots[i].flags & flag))
                release(&t->slots[i]);
    }

    void close_all(handle_table* t)
    {
        for (sint32_t i = 0; i < HANDLE_TABLE_SIZE; i++)
            if (t->slots[i].obj)
                release(&t->slots[i]);
    }
}
