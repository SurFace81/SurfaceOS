// Events and waiting on kernel objects. See obj/event.h.

#include "../../include/obj/event.h"
#include "../../include/cpu/wait.h"
#include "../../include/drivers/pit.h"
#include "../../sdk/include/abi/errno.h"

namespace
{
    struct event_obj
    {
        kobject    hdr;         // type Event
        volatile bool is_set;
        bool       auto_reset;
        wait_queue waiters;
        bool       used;        // pool slot taken
    };

    const uint32_t MAX_EVENTS = 256;     // every mutex of every program is one
    event_obj pool[MAX_EVENTS];

    event_obj* as_event(kobject* o)
    {
        return (event_obj*)o;
    }

    void ev_destroy(kobject* o)
    {
        as_event(o)->used = false;
    }

    bool ev_signaled(kobject* o)
    {
        return as_event(o)->is_set;
    }

    wait_queue* ev_waitq(kobject* o)
    {
        return &as_event(o)->waiters;
    }

    void ev_consume(kobject* o)
    {
        event_obj* e = as_event(o);
        if (e->auto_reset)
            e->is_set = false;
    }

    const kobject_ops event_ops =
    {
        obj_type::Event, "event", ev_destroy,
        ev_signaled, ev_waitq, ev_consume,
    };

    bool signaled_cond(void* arg)
    {
        kobject* o = (kobject*)arg;
        return o->ops->signaled(o);
    }
}

namespace event
{
    kobject* create(bool auto_reset)
    {
        for (uint32_t i = 0; i < MAX_EVENTS; i++)
        {
            event_obj* e = &pool[i];
            if (e->used)
                continue;
            kobj::init(&e->hdr, &event_ops);
            e->is_set = false;
            e->auto_reset = auto_reset;
            e->waiters.head = nullptr;
            e->used = true;
            return &e->hdr;
        }
        return nullptr;
    }

    void set(kobject* o)
    {
        if (kobj::type(o) != obj_type::Event)
            return;
        as_event(o)->is_set = true;
        wait::wake_up(&as_event(o)->waiters);
    }

    void reset(kobject* o)
    {
        if (kobj::type(o) == obj_type::Event)
            as_event(o)->is_set = false;
    }
}

namespace objects
{
    sint64_t wait(kobject* o, uint64_t tick)
    {
        const kobject_ops* ops = o->ops;
        if (!ops->signaled || !ops->waitq)
            return -EINVAL;

        // wait_event also returns once the deadline passes; only the
        // object's own state tells the two apart. The kernel does not
        // preempt itself, so nothing can take the signal between this check
        // and consume().
        if (!wait::wait_event(ops->waitq(o), signaled_cond, o, tick))
            return -EINTR;
        if (!ops->signaled(o))
            return -ETIMEDOUT;
        if (ops->consume)
            ops->consume(o);
        return 0;
    }

    sint64_t wait_handle(handle_table* t, sint32_t h, uint64_t tick)
    {
        sint64_t rc = 0;
        kobject* o = handles::get(t, h, obj_type::None, &rc);
        if (!o)
            return rc;
        // An own reference: the handle may be closed while this sleeps.
        kobj::get(o);
        rc = wait(o, tick);
        kobj::put(o);
        return rc;
    }
}
