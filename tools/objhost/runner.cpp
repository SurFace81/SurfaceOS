// Unit tests for kernel objects and handle tables (src/kernel/obj/object.cpp).
// Run with tools/objtest_host.sh.
//
// object.cpp has no kernel dependencies, so no stub file is needed.

#include "../../src/include/obj/object.h"
#include "../../src/sdk/include/abi/errno.h"

extern "C" int printf(const char*, ...);

static int passed = 0;
static int failed = 0;

static void check(const char* name, bool ok)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", name);
    if (ok) passed++; else failed++;
}

static void section(const char* s) { printf("\n%s\n", s); }

// A test object that counts how often it was destroyed.
struct test_obj
{
    kobject hdr;
    int     destroyed;
};

static void test_destroy(kobject* o)
{
    ((test_obj*)o)->destroyed++;
}

static const kobject_ops file_ops  = { obj_type::File,  "file",  test_destroy };
static const kobject_ops event_ops = { obj_type::Event, "event", test_destroy };

static void make(test_obj* t, const kobject_ops* ops)
{
    kobj::init(&t->hdr, ops);
    t->destroyed = 0;
}

static void t_refcount()
{
    section("reference counting");

    test_obj a;
    make(&a, &file_ops);
    check("the creator holds one reference", a.hdr.refcnt == 1);
    kobj::get(&a.hdr);
    kobj::put(&a.hdr);
    check("get + put keeps it alive", a.destroyed == 0 && a.hdr.refcnt == 1);
    kobj::put(&a.hdr);
    check("the last put destroys it once", a.destroyed == 1);
    kobj::put(&a.hdr);
    check("a put after that does nothing", a.destroyed == 1);
    check("type() reports the ops type", kobj::type(&a.hdr) == obj_type::File);
}

static void t_install()
{
    section("install / get / close");

    handle_table t;
    handles::init(&t);
    test_obj a;
    make(&a, &file_ops);

    sint32_t h = -1;
    check("install takes the lowest slot", handles::install(&t, &a.hdr, 0, 0, &h) == 0 && h == 0);
    check("... and a reference", a.hdr.refcnt == 2);

    sint32_t h2 = -1;
    handles::install(&t, &a.hdr, 0, 0, &h2);
    check("the next install gets slot 1", h2 == 1);

    sint32_t h3 = -1;
    check("min is honoured", handles::install(&t, &a.hdr, 0, 10, &h3) == 0 && h3 == 10);

    sint64_t rc = 1;
    kobject* o = handles::get(&t, 0, obj_type::File, &rc);
    check("get returns the object", o == &a.hdr && rc == 0);
    check("get borrows: no reference taken", a.hdr.refcnt == 4);
    check("get with None accepts any type", handles::get(&t, 1, obj_type::None, &rc) == &a.hdr);
    check("get of another type fails EBADF",
          handles::get(&t, 0, obj_type::Event, &rc) == nullptr && rc == -EBADF);
    check("get of a free slot fails EBADF",
          handles::get(&t, 5, obj_type::None, &rc) == nullptr && rc == -EBADF);
    check("get out of range fails EBADF",
          handles::get(&t, -1, obj_type::None, &rc) == nullptr && rc == -EBADF &&
          handles::get(&t, HANDLE_TABLE_SIZE, obj_type::None, &rc) == nullptr);

    check("close releases the slot", handles::close(&t, 1) == 0 && a.hdr.refcnt == 3);
    check("closing it again fails EBADF", handles::close(&t, 1) == -EBADF);
    sint32_t h4 = -1;
    handles::install(&t, &a.hdr, 0, 0, &h4);
    check("a freed slot is reused first", h4 == 1);

    handles::close_all(&t);
    check("close_all drops every reference", a.hdr.refcnt == 1 && a.destroyed == 0);
    kobj::put(&a.hdr);
    check("the creator's put destroys it", a.destroyed == 1);
}

static void t_full()
{
    section("a full table");

    handle_table t;
    handles::init(&t);
    test_obj a;
    make(&a, &file_ops);

    int n = 0;
    sint32_t h;
    while (handles::install(&t, &a.hdr, 0, 0, &h) == 0)
        n++;
    check("exactly HANDLE_TABLE_SIZE handles fit", n == HANDLE_TABLE_SIZE);
    check("the next install fails EMFILE", handles::install(&t, &a.hdr, 0, 0, &h) == -EMFILE);
    check("a failed install takes no reference", a.hdr.refcnt == 1 + HANDLE_TABLE_SIZE);
    check("lowest_free says -1", handles::lowest_free(&t, 0) == -1);
    check("a bad min fails EINVAL", handles::install(&t, &a.hdr, 0, -1, &h) == -EINVAL);
    handles::close_all(&t);
    check("close_all empties it", a.hdr.refcnt == 1);
}

static void t_install_at()
{
    section("install_at");

    handle_table t;
    handles::init(&t);
    test_obj a, b;
    make(&a, &file_ops);
    make(&b, &event_ops);

    check("install_at a free slot", handles::install_at(&t, &a.hdr, 0, 7) == 0 && a.hdr.refcnt == 2);
    check("install_at an occupied slot closes the old object",
          handles::install_at(&t, &b.hdr, 0, 7) == 0 && a.hdr.refcnt == 1 && b.hdr.refcnt == 2);
    check("install_at the same object keeps it alive",
          handles::install_at(&t, &b.hdr, 0, 7) == 0 && b.hdr.refcnt == 2 && b.destroyed == 0);
    check("install_at out of range fails EBADF", handles::install_at(&t, &a.hdr, 0, -3) == -EBADF);
    handles::close_all(&t);
    check("afterwards only the creators hold them", a.hdr.refcnt == 1 && b.hdr.refcnt == 1);
}

static void t_flags_fork_exec()
{
    section("flags, fork, execve");

    handle_table parent, child;
    handles::init(&parent);
    test_obj a, b;
    make(&a, &file_ops);
    make(&b, &event_ops);

    sint32_t ha, hb;
    handles::install(&parent, &a.hdr, 0, 0, &ha);
    handles::install(&parent, &b.hdr, HANDLE_CLOEXEC, 0, &hb);
    check("flags are stored per slot",
          handles::get_flags(&parent, ha) == 0 && handles::get_flags(&parent, hb) == HANDLE_CLOEXEC);
    check("set_flags changes them",
          handles::set_flags(&parent, ha, HANDLE_CLOEXEC) == 0 &&
          handles::get_flags(&parent, ha) == HANDLE_CLOEXEC);
    handles::set_flags(&parent, ha, 0);
    check("flags of a free slot fail EBADF",
          handles::get_flags(&parent, 30) == -EBADF && handles::set_flags(&parent, 30, 0) == -EBADF);

    handles::fork(&child, &parent);
    check("fork copies every slot with a reference",
          a.hdr.refcnt == 3 && b.hdr.refcnt == 3 &&
          handles::get_flags(&child, hb) == HANDLE_CLOEXEC);

    handles::close_flagged(&child, HANDLE_CLOEXEC);
    sint64_t rc;
    check("execve closes the CLOEXEC slots only",
          handles::get(&child, hb, obj_type::None, &rc) == nullptr &&
          handles::get(&child, ha, obj_type::None, &rc) == &a.hdr && b.hdr.refcnt == 2);

    handles::close_all(&child);
    handles::close_all(&parent);
    kobj::put(&a.hdr);
    kobj::put(&b.hdr);
    check("everything is destroyed exactly once", a.destroyed == 1 && b.destroyed == 1);
}

int main()
{
    printf("kernel objects and handle tables\n");

    t_refcount();
    t_install();
    t_full();
    t_install_at();
    t_flags_fork_exec();

    printf("\nobjtest: %d passed, %d failed\n", passed, failed);
    return failed;
}
