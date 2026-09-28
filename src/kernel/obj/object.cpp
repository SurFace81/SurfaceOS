// Kernel objects and handle tables. See obj/object.h.

#include "../../include/obj/object.h"
#include "../../include/errno.h"

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
            if (o)
                kobj::put(o);
        }
    }

    void init(handle_table* t)
    {
        for (sint32_t i = 0; i < HANDLE_TABLE_SIZE; i++)
            t->slots[i].obj = nullptr;
    }

    sint64_t install(handle_table* t, kobject* o, sint32_t* out)
    {
        for (sint32_t h = 0; h < HANDLE_TABLE_SIZE; h++)
        {
            if (t->slots[h].obj)
                continue;
            kobj::get(o);
            t->slots[h].obj = o;
            *out = h;
            return 0;
        }
        return -EMFILE;
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

    void close_all(handle_table* t)
    {
        for (sint32_t i = 0; i < HANDLE_TABLE_SIZE; i++)
            if (t->slots[i].obj)
                release(&t->slots[i]);
    }
}
