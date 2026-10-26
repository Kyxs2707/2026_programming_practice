#include "frontend/driver.hpp"
#include "frontend/flex_allocations.hpp"
#include "semantic/checker.hpp"
#include "test_support.hpp"
#include <fstream>
#include <iterator>
#include <limits>
using namespace myb;
namespace {
void check_text(std::string text) {
    EmergencyFailure first;
    SourceManager sources(first);
    Driver driver(sources, sources.add("checker", std::move(text)));
    auto tree = driver.parse();
    Checker checker(first);
    checker.check(*tree);
}
void rejects(std::string text, FailureDomain domain, ReasonCode reason) {
    try {
        check_text(std::move(text));
        REQUIRE(false);
    } catch (const FrontendFailure &failure) {
        REQUIRE(failure.header().domain == domain);
        REQUIRE(failure.header().reason == reason);
    }
}
} // namespace
MYB_TEST(checker_contract) {
    check_text("if(false){missing(1,2);} len(); let x=x+1;");
    check_text("fn f(a){return g(a);} fn g(a){return f(a);} struct A { b:B?; } struct B { a:A?; }");
    check_text("print = write(1); Int64 = 2; unknown = 3; P{bad: 1, bad: 2};");
    check_text(
        "fn f(a){{let a=1;} return;} for(let x in x){{let x=1;}} {} struct Empty{} Empty{};");
    check_text(
        "(-9223372036854775808); -(-9223372036854775808); - #comment\n0009223372036854775808;");
    rejects("-(9223372036854775808);", FailureDomain::Frontend, ReasonCode::Range);
    rejects("-9223372036854775808[0];", FailureDomain::Frontend, ReasonCode::Range);
    for (auto text :
         {"let x; let x;", "fn f(a,a){}", "fn f(a){let a;}", "for(let x in []){let x;}",
          "let print;", "fn len(){}", "struct Int64{}", "fn f(){} struct f{}", "let f; fn f(){}",
          "fn f(){} let f;", "fn f(){} fn f(){}", "struct P{a:Int64;a:String;}",
          "struct P{a:Null;}", "struct P{a:Missing;}", "fn f(){let String;}", "fn f(print){}"})
        rejects(text, FailureDomain::Structure, ReasonCode::Declaration);
    for (auto text :
         {"return;", "if(false){break;}", "fn f(){continue;}", "(1+2)=3;", "[1][:]=[];", "f()=1;"})
        rejects(text, FailureDomain::Structure, ReasonCode::Syntax);
    check_text("let x; ((x))=1; (make()[0])=2; obj.field=3; arr[0].field=4;");
}
MYB_TEST(frontend_parse) {
    std::ifstream input("tests/fixtures/frontend/all_syntax.myb", std::ios::binary);
    REQUIRE(input.good());
    std::string text((std::istreambuf_iterator<char>(input)), {});
    EmergencyFailure first;
    SourceManager sources(first);
    Driver driver(sources, sources.add("all_syntax", text));
    auto tree = driver.parse();
    REQUIRE(tree->items().size() == 5);
    auto &fn = dynamic_cast<const FunctionDecl &>(tree->items()[1]);
    REQUIRE(fn.parameters().size() == 2);
    auto &assignment = dynamic_cast<const AssignStmt &>(fn.body().statements()[1]);
    REQUIRE(dynamic_cast<const GroupExpr *>(&assignment.target()));
    auto &condition = dynamic_cast<const IfStmt &>(fn.body().statements()[2]);
    auto &logical = dynamic_cast<const LogicalExpr &>(condition.condition());
    REQUIRE(logical.op() == LogicalOp::Or);
    REQUIRE(dynamic_cast<const LogicalExpr &>(logical.right()).op() == LogicalOp::And);
    auto &block = dynamic_cast<const BlockStmt &>(tree->items()[3]);
    auto &slice = dynamic_cast<const SliceExpr &>(
        *dynamic_cast<const LetStmt &>(block.statements()[0]).initializer());
    REQUIRE(slice.start() == nullptr);
    REQUIRE(slice.end() == nullptr);
    Checker checker(first);
    auto checked = checker.check(*tree);
    REQUIRE(!checked.literals.empty());
    for (auto valid : {"{} fn f(){} struct S{} S{};",
                       "let a=[1]; -a[0]; a[:]; a[null:]; a[:null]; a[null:null];",
                       "1 < 2 && 3 >= 2 || 1 != 2; (1<2)==true; 2<=3; 3>2;"})
        check_text(valid);
    for (auto bad :
         {"1<2<3;", "1==2!=3;", "fn f(a,){}", "struct P{a:Int64??;}", "[1,];", "P{a:1,};",
          "if(true){};", "fn f(){};", "struct S{};", "a=b=c;", "a[::];", "(f)();", "a.f();",
          "fn f(){fn g(){}}", "{struct P{}}", "if true {}", "for(x in []){}"})
        rejects(bad, FailureDomain::Frontend, ReasonCode::Syntax);
}
MYB_TEST(checker_index_and_traversal) {
    EmergencyFailure first;
    SourceManager sources(first);
    Driver driver(
        sources,
        sources.add(
            "index",
            "fn f(){} struct A { a: Int64; b: Float64?; c: Bool; d: String; e: Array; f:B?; } "
            "fn g(){} struct B { a:A; }"));
    auto tree = driver.parse();
    Checker checker(first);
    auto checked = checker.check(*tree);
    REQUIRE(checked.declarations.functions.size() == 2);
    REQUIRE(checked.declarations.functions[0]->name() == "f");
    REQUIRE(checked.declarations.functions[1]->name() == "g");
    REQUIRE(checked.declarations.structs.size() == 2);
    REQUIRE(checked.declarations.structs[0]->name() == "A");
    REQUIRE(checked.declarations.structs[1]->name() == "B");
    const auto &types = checked.declarations.field_types;
    REQUIRE(types.size() == 2);
    REQUIRE(types[0].size() == 6);
    REQUIRE(types[0][0].kind == TypeKind::Int64);
    REQUIRE(!types[0][0].nullable);
    REQUIRE(types[0][1].kind == TypeKind::Float64);
    REQUIRE(types[0][1].nullable);
    REQUIRE(types[0][2].kind == TypeKind::Bool);
    REQUIRE(types[0][3].kind == TypeKind::String);
    REQUIRE(types[0][4].kind == TypeKind::Array);
    REQUIRE(types[0][5].kind == TypeKind::Struct);
    REQUIRE(types[0][5].schema_id == 1);
    REQUIRE(types[0][5].nullable);
    REQUIRE(types[1][0].schema_id == 0);
    for (auto name :
         {"len",    "append",    "insert",     "pop",    "remove_at", "deep_copy", "print",
          "write",  "read_line", "int",        "float",  "str",       "try_int",   "try_float",
          "assert", "read_text", "write_text", "split",  "join",      "trim",      "split_lines",
          "Int64",  "Float64",   "Bool",       "String", "Array"}) {
        rejects(std::string("let ") + name + ";", FailureDomain::Structure,
                ReasonCode::Declaration);
        rejects(std::string("fn f(") + name + "){}", FailureDomain::Structure,
                ReasonCode::Declaration);
        rejects(std::string("for(let ") + name + " in []){}", FailureDomain::Structure,
                ReasonCode::Declaration);
    }
    // 每个表达式容器都必须访问其孩子，未执行路径也检查字面量范围。
    for (auto text :
         {"fn f(){return 9223372036854775808;}", "if(false){let x=9223372036854775808;}",
          "while(false){9223372036854775808;}", "for(let x in 9223372036854775808){}",
          "for(let x in []){9223372036854775808;}", "false && 9223372036854775808;",
          "true || 9223372036854775808;", "[9223372036854775808];",
          "Unknown{x:9223372036854775808};", "9223372036854775808[0];", "x[9223372036854775808];",
          "x[9223372036854775808:];", "x[:9223372036854775808];", "f(9223372036854775808);",
          "x=9223372036854775808;", "x[9223372036854775808]=1;", "if(9223372036854775808){}",
          "if(false){}else{9223372036854775808;}", "let x=1e999;"})
        rejects(text, FailureDomain::Frontend, ReasonCode::Range);
    check_text(
        "struct P{print:Int64;String:String;} {let x;}{let x;} fn f(){let x;} fn g(){let x;}");
    check_text("fn f(){while(false){if(false){continue;}break;}} while(false){break;} for(let a in "
               "[]){continue;}");
}
MYB_TEST(frontend_shapes_and_spans) {
    EmergencyFailure first;
    SourceManager sources(first);
    const std::string text = "-a[0].x; a[:]; a[null:]; a[:null]; a[null:null];";
    Driver driver(sources, sources.add("shapes", text));
    auto tree = driver.parse();
    auto expression = [&](std::size_t i) -> const Expr & {
        return dynamic_cast<const ExprStmt &>(tree->items()[i]).expr();
    };
    auto &unary = dynamic_cast<const UnaryExpr &>(expression(0));
    auto &field = dynamic_cast<const FieldExpr &>(unary.operand());
    REQUIRE(dynamic_cast<const IndexExpr *>(&field.receiver()));
    REQUIRE(unary.operator_span().begin == 0);
    REQUIRE(unary.operator_span().end == 1);
    REQUIRE(field.access_span().begin == 5);
    REQUIRE(field.name_span().begin == 6);
    auto &index = dynamic_cast<const IndexExpr &>(field.receiver());
    REQUIRE(index.access_span().begin == 2);
    auto &omitted = dynamic_cast<const SliceExpr &>(expression(1));
    REQUIRE(omitted.start() == nullptr && omitted.end() == nullptr);
    auto &start = dynamic_cast<const SliceExpr &>(expression(2));
    REQUIRE(dynamic_cast<const NullExpr *>(start.start()));
    REQUIRE(start.end() == nullptr);
    auto &end = dynamic_cast<const SliceExpr &>(expression(3));
    REQUIRE(end.start() == nullptr);
    REQUIRE(dynamic_cast<const NullExpr *>(end.end()));
    auto &both = dynamic_cast<const SliceExpr &>(expression(4));
    REQUIRE(dynamic_cast<const NullExpr *>(both.start()));
    REQUIRE(dynamic_cast<const NullExpr *>(both.end()));
    auto id = sources.add("construct", "Pair{a:1,a:2}; let x; fn f(){return;}");
    Driver construct_driver(sources, id);
    auto other = construct_driver.parse();
    auto &construct = dynamic_cast<const ConstructExpr &>(
        dynamic_cast<const ExprStmt &>(other->items()[0]).expr());
    REQUIRE(construct.fields().size() == 2);
    REQUIRE(construct.fields()[0].name() == "a" && construct.fields()[1].name() == "a");
    REQUIRE(dynamic_cast<const LetStmt &>(other->items()[1]).initializer() == nullptr);
    REQUIRE(dynamic_cast<const ReturnStmt &>(
                dynamic_cast<const FunctionDecl &>(other->items()[2]).body().statements()[0])
                .value() == nullptr);
    auto empty_id = sources.add("empty", "");
    Driver empty_driver(sources, empty_id);
    auto empty = empty_driver.parse();
    auto checked = Checker(first).check(*empty);
    REQUIRE(checked.declarations.functions.empty());
    REQUIRE(checked.declarations.structs.empty());
    REQUIRE(checked.literals.empty());
}
MYB_TEST(frontend_failure_cleanup_and_locations) {
    struct Input {
        std::string text;
        std::size_t begin;
        std::size_t end;
    };
    const Input bad_inputs[] = {
        {"fn f(){let x=@;}", 13, 14},  {"let a=[1,];", 9, 10},
        {"let s='bad\\q';", 10, 12},   {"fn f(){'unterminated", 7, 20},
        {"for(let x in []){", 17, 17}, {"let x=1;\r\n\xF0\x9F\x98\x80;", 10, 14}};
    for (const auto &input : bad_inputs) {
        EmergencyFailure first;
        SourceManager sources(first);
        auto id = sources.add("bad", input.text);
        Driver driver(sources, id);
        try {
            driver.parse();
            REQUIRE(false);
        } catch (const FrontendFailure &failure) {
            REQUIRE(failure.header().domain == FailureDomain::Frontend);
            REQUIRE(failure.header().reason == ReasonCode::Syntax);
            REQUIRE(failure.header().span->source == id);
            REQUIRE(failure.header().span->begin == input.begin);
            REQUIRE(failure.header().span->end == input.end);
        }
        REQUIRE(FlexAllocationLedger::live_total() == 0);
        auto valid = sources.add("valid", "fn f(){return 1;} f();");
        Driver again(sources, valid);
        REQUIRE(again.parse()->items().size() == 2);
        REQUIRE(FlexAllocationLedger::live_total() == 0);
    }
}
