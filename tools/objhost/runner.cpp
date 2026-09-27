// Unit tests for kernel objects and handle tables (src/kernel/obj/object.cpp).
// Run with tools/objtest_host.sh.
//
// object.cpp has no kernel dependencies, so no stub file is needed.

#include "../../src/include/obj/object.h"
#include "../../src/include/errno.h"

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

static const kobject_ops file_ops  = { obj_type::File,  "file",  test_destroy,
                                       nullptr, nullptr, nullptr };
static const kobject_ops event_ops = { obj_type::Event, "event", test_destroy,
                                       nullptr, nullptr, nullptr };

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
    check("install takes the lowest slot", handles::install(&t, &a.hdr, &h) == 0 && h == 0);
    check("... and a reference", a.hdr.refcnt == 2);

    sint32_t h2 = -1;
    handles::install(&t, &a.hdr, &h2);
    check("the next install gets slot 1", h2 == 1);

    sint32_t h3 = -1;
    check("and the one after slot 2", handles::install(&t, &a.hdr, &h3) == 0 && h3 == 2);

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
    handles::install(&t, &a.hdr, &h4);
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
    while (handles::install(&t, &a.hdr, &h) == 0)
        n++;
    check("exactly HANDLE_TABLE_SIZE handles fit", n == HANDLE_TABLE_SIZE);
    check("the next install fails EMFILE", handles::install(&t, &a.hdr, &h) == -EMFILE);
    check("a failed install takes no reference", a.hdr.refcnt == 1 + HANDLE_TABLE_SIZE);
    handles::close_all(&t);
    check("close_all empties it", a.hdr.refcnt == 1);
}

int main()
{
    printf("kernel objects and handle tables\n");

    t_refcount();
    t_install();
    t_full();

    printf("\nobjtest: %d passed, %d failed\n", passed, failed);
    return failed;
}
