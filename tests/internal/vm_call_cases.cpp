#include "fixtures.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
using namespace myb;

MYB_TEST(vm_calls) {
    VmFixture f;
    auto r = f.run("fn add(a,b){return a+b;}print(100+add(3,4));");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "107\n");
    r = f.run("fn f(){}fn g(){return;}print(f());print(g());");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "null\nnull\n");
    r = f.run("fn f(x){}f(write(\"X\"),write(\"Y\"));");
    REQUIRE(r.exit_code == 1 && r.stdout_bytes.empty() && r.failure->reason == ReasonCode::Type);
    r = f.run("fn f(a,b){}f(write(\"A\"),missing);");
    REQUIRE(r.exit_code == 1 && r.stdout_bytes == "A" && r.failure->reason == ReasonCode::Name);
    REQUIRE(r.call_names.empty());
    r = f.run("let x=1;fn f(){return x;}fn caller(){let x=2;return f();}print(caller());");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "1\n");
    r = f.run("f();let x=1;fn f(){print(x);}");
    REQUIRE(r.exit_code == 1 && r.failure->reason == ReasonCode::Name);
    r = f.run("print(even(12));fn even(n){if(n==0){return true;}return odd(n-1);}fn "
              "odd(n){if(n==0){return false;}return even(n-1);}");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "true\n");
    r = f.run("fn count(n){if(n==0){return 0;}return 1+count(n-1);}print(count(4096));");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "4096\n");
    TestHooks::force_gc(true);
    r = f.run("fn tag(){return \"北\"+\"邮\";}let x=tag();print(\"(\"+tag()+\")\");print(x);");
    TestHooks::force_gc(false);
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "(北邮)\n北邮\n");
    r = f.run("fn a(){b();}fn b(){print(1/0);}a();");
    REQUIRE(r.exit_code == 1 && r.call_names == std::vector<std::string>({"b", "a"}));
    r = f.run("fn f(x){x=9;return x;}let x=2;print(f(x));print(x);");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "9\n2\n");
}
