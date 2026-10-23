#include "fixtures.hpp"
#include "test_hooks.hpp"
#include "test_support.hpp"
using namespace myb;

MYB_TEST(vm_scalar) {
    VmFixture f;
    auto r = f.run(
        "let x=3;{print(x);let x;x=7;print(x);}if(false){print(missing);}print(false&&missing);");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "3\n7\nfalse\n");
    r = f.run("let x=1;{let x=x+1;}");
    REQUIRE(r.exit_code == 1 && r.failure->reason == ReasonCode::Uninitialized);
    r = f.run("print(x);let x=1;");
    REQUIRE(r.exit_code == 1 && r.failure->reason == ReasonCode::Name);
    r = f.run("let x;x=null;print(x);x=\"a\";print(x);");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "null\na\n");
    r = f.run("print(true+missing);");
    REQUIRE(r.exit_code == 1 && r.failure->reason == ReasonCode::Name);
    r = f.run("print(1&&true);");
    REQUIRE(r.exit_code == 1 && r.failure->reason == ReasonCode::Type);
    r = f.run("let n=0;while(n<8){n=n+1;if(n==2){continue;}if(n==5){break;}print(n);}print(n);");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "1\n3\n4\n5\n");
    r = f.run("let x=1;{x=2;let y=3;{let x=4;print(x);}}print(x);print(y);");
    REQUIRE(r.exit_code == 1 && r.stdout_bytes == "4\n2\n" &&
            r.failure->reason == ReasonCode::Name);
    r = f.run(
        "print(true||missing);print(false||true);print(true&&false);while(false){print(missing);}");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "true\ntrue\nfalse\n");
    r = f.run("let n=0;while(n<3){let x;n=n+1;x=n;print(x);}");
    REQUIRE(r.exit_code == 0 && r.stdout_bytes == "1\n2\n3\n");
    r = f.run("print=write(\"R\");");
    REQUIRE(r.exit_code == 1 && r.stdout_bytes == "R" && r.failure->reason == ReasonCode::Name);
}

MYB_TEST(vm_growth_failure) {
    for (auto point : {FaultPoint::VmValues, FaultPoint::VmStates, FaultPoint::VmFrames}) {
        VmFixture f;
        TestHooks::fail_next(point);
        auto r = f.run("let x=1;print(x);");
        TestHooks::clear_fault();
        REQUIRE(r.exit_code == 3 && r.failure->domain == FailureDomain::Resource);
        REQUIRE(r.stdout_bytes.empty());
    }
}
