%skeleton "lalr1.cc"
%require "3.8"
%define api.namespace {myb}
%define api.parser.class {Parser}
%define api.value.type variant
%define api.token.constructor
%define parse.error detailed
%locations
%define api.location.type {myb::SourceSpan}
%parse-param {myb::Driver& driver}
%lex-param {myb::Driver& driver}
%code requires {
#include "frontend/driver.hpp"
// 非空归约覆盖首尾符号；空归约沿用前一符号末尾的空范围。
#define YYLLOC_DEFAULT(Current,Rhs,N) do { if(N){ (Current)={YYRHSLOC(Rhs,1).source,YYRHSLOC(Rhs,1).begin,YYRHSLOC(Rhs,N).end};}else{(Current)={YYRHSLOC(Rhs,0).source,YYRHSLOC(Rhs,0).end,YYRHSLOC(Rhs,0).end};}}while(false)
}
%code provides {namespace myb {
/**
 * @brief 将 Bison 的词法请求交给当前驱动的扫描器。
 * @param[in] driver 持有本次解析状态的驱动。
 * @return 拥有载荷并携带源码范围的下一个符号。
 */
Parser::symbol_type yylex(Driver&);}}
%initial-action { @$={driver.source_id(),0,0}; }
%token <std::string> IDENTIFIER STRING
%token <myb::IntLexeme> INT
%token <myb::FloatLexeme> FLOAT
%token TRUE FALSE NULL_VALUE LET FN STRUCT IF ELSE WHILE FOR IN RETURN BREAK CONTINUE
%token PLUS "+" MINUS "-" STAR "*" SLASH "/" FLOOR "//" PERCENT "%" NOT "!"
%token LESS "<" LESS_EQUAL "<=" GREATER ">" GREATER_EQUAL ">=" EQUAL "==" NOT_EQUAL "!="
%token AND "&&" OR "||" ASSIGN "=" DOT "." COLON ":" QUESTION "?"
%token LPAREN "(" RPAREN ")" LBRACE "{" RBRACE "}" LBRACKET "[" RBRACKET "]" COMMA "," SEMICOLON ";"
%type <std::vector<std::unique_ptr<myb::TopItem>>> items
%type <std::unique_ptr<myb::TopItem>> top_item
%type <std::unique_ptr<myb::Stmt>> statement if_statement else_body
%type <std::unique_ptr<myb::BlockStmt>> block
%type <std::vector<std::unique_ptr<myb::Stmt>>> statements
%type <std::unique_ptr<myb::Expr>> expression logical_or logical_and comparison additive multiplicative unary postfix primary optional_expression initializer
%type <std::vector<std::unique_ptr<myb::Expr>>> arguments argument_list
%type <std::vector<myb::Parameter>> parameters parameter_list
%type <std::vector<myb::FieldDecl>> field_declarations
%type <myb::TypeRef> type_ref
%type <bool> nullable
%type <std::vector<myb::FieldInit>> fields field_list
%type <myb::CompareOp> compare_operator
%start program
// 每层产生式对应一个优先级；比较最多一个操作符，访问后缀高于一元运算。
%%
program: items {driver.accept(driver.factory().make<myb::AstProgram>(@$,std::move($1)));};
items: %empty {$$=std::vector<std::unique_ptr<myb::TopItem>>{};}
 | items top_item {$$=std::move($1);$$.push_back(std::move($2));};
top_item: statement {$$=std::move($1);}
 | FN IDENTIFIER LPAREN parameters RPAREN block {$$=driver.factory().make<myb::FunctionDecl>(@$,std::move($2),@2,std::move($4),std::move($6));}
 | STRUCT IDENTIFIER LBRACE field_declarations RBRACE {$$=driver.factory().make<myb::StructDecl>(@$,std::move($2),@2,std::move($4));};
parameters: %empty {$$=std::vector<myb::Parameter>{};} | parameter_list {$$=std::move($1);};
parameter_list: IDENTIFIER {$$=std::vector<myb::Parameter>{};$$.push_back({std::move($1),@1});}
 | parameter_list COMMA IDENTIFIER {$$=std::move($1);$$.push_back({std::move($3),@3});};
field_declarations: %empty {$$=std::vector<myb::FieldDecl>{};}
 | field_declarations IDENTIFIER COLON type_ref SEMICOLON {$$=std::move($1);$$.push_back({std::move($2),@2,std::move($4)});};
type_ref: IDENTIFIER nullable {$$={std::move($1),$2,@$};};
nullable: %empty {$$=false;} | QUESTION {$$=true;};
block: LBRACE statements RBRACE {$$=driver.factory().make<myb::BlockStmt>(@$,std::move($2));};
statements: %empty {$$=std::vector<std::unique_ptr<myb::Stmt>>{};}
 | statements statement {$$=std::move($1);$$.push_back(std::move($2));};
statement: expression SEMICOLON {$$=driver.factory().make<myb::ExprStmt>(@$,std::move($1));}
 | LET IDENTIFIER initializer SEMICOLON {$$=driver.factory().make<myb::LetStmt>(@$,std::move($2),@2,std::move($3));}
 | expression ASSIGN expression SEMICOLON {$$=driver.factory().make<myb::AssignStmt>(@$,std::move($1),std::move($3),@2);}
 | block {$$=std::move($1);}
 | if_statement {$$=std::move($1);}
 | WHILE LPAREN expression RPAREN block {$$=driver.factory().make<myb::WhileStmt>(@$,std::move($3),std::move($5));}
 | FOR LPAREN LET IDENTIFIER IN expression RPAREN block {$$=driver.factory().make<myb::ForStmt>(@$,std::move($4),@4,std::move($6),std::move($8));}
 | RETURN optional_expression SEMICOLON {$$=driver.factory().make<myb::ReturnStmt>(@$,std::move($2));}
 | BREAK SEMICOLON {$$=driver.factory().make<myb::BreakStmt>(@$);}
 | CONTINUE SEMICOLON {$$=driver.factory().make<myb::ContinueStmt>(@$);};
initializer: %empty {$$=nullptr;} | ASSIGN expression {$$=std::move($2);};
optional_expression: %empty {$$=nullptr;} | expression {$$=std::move($1);};
if_statement: IF LPAREN expression RPAREN block else_body {$$=driver.factory().make<myb::IfStmt>(@$,std::move($3),std::move($5),std::move($6));};
else_body: %empty {$$=nullptr;} | ELSE block {$$=std::move($2);} | ELSE if_statement {$$=std::move($2);};
arguments: %empty {$$=std::vector<std::unique_ptr<myb::Expr>>{};} | argument_list {$$=std::move($1);};
argument_list: expression {$$=std::vector<std::unique_ptr<myb::Expr>>{};$$.push_back(std::move($1));}
 | argument_list COMMA expression {$$=std::move($1);$$.push_back(std::move($3));};
fields: %empty {$$=std::vector<myb::FieldInit>{};} | field_list {$$=std::move($1);};
field_list: IDENTIFIER COLON expression {$$=std::vector<myb::FieldInit>{};$$.emplace_back(std::move($1),@1,std::move($3));}
 | field_list COMMA IDENTIFIER COLON expression {$$=std::move($1);$$.emplace_back(std::move($3),@3,std::move($5));};
expression: logical_or {$$=std::move($1);};
logical_or: logical_and {$$=std::move($1);}
 | logical_or OR logical_and {$$=driver.factory().make<myb::LogicalExpr>(@$,myb::LogicalOp::Or,@2,std::move($1),std::move($3));};
logical_and: comparison {$$=std::move($1);}
 | logical_and AND comparison {$$=driver.factory().make<myb::LogicalExpr>(@$,myb::LogicalOp::And,@2,std::move($1),std::move($3));};
comparison: additive {$$=std::move($1);}
 | additive compare_operator additive {$$=driver.factory().make<myb::BinaryExpr>(@$,myb::ArithmeticOrCompare{$2},@2,std::move($1),std::move($3));};
compare_operator: LESS {$$=myb::CompareOp::Less;} | LESS_EQUAL {$$=myb::CompareOp::LessEqual;}
 | GREATER {$$=myb::CompareOp::Greater;} | GREATER_EQUAL {$$=myb::CompareOp::GreaterEqual;}
 | EQUAL {$$=myb::CompareOp::Equal;} | NOT_EQUAL {$$=myb::CompareOp::NotEqual;};
additive: multiplicative {$$=std::move($1);}
 | additive PLUS multiplicative {$$=driver.factory().make<myb::BinaryExpr>(@$,myb::ArithmeticOrCompare{myb::BinaryOp::Add},@2,std::move($1),std::move($3));}
 | additive MINUS multiplicative {$$=driver.factory().make<myb::BinaryExpr>(@$,myb::ArithmeticOrCompare{myb::BinaryOp::Subtract},@2,std::move($1),std::move($3));};
multiplicative: unary {$$=std::move($1);}
 | multiplicative STAR unary {$$=driver.factory().make<myb::BinaryExpr>(@$,myb::ArithmeticOrCompare{myb::BinaryOp::Multiply},@2,std::move($1),std::move($3));}
 | multiplicative SLASH unary {$$=driver.factory().make<myb::BinaryExpr>(@$,myb::ArithmeticOrCompare{myb::BinaryOp::Divide},@2,std::move($1),std::move($3));}
 | multiplicative FLOOR unary {$$=driver.factory().make<myb::BinaryExpr>(@$,myb::ArithmeticOrCompare{myb::BinaryOp::FloorDivide},@2,std::move($1),std::move($3));}
 | multiplicative PERCENT unary {$$=driver.factory().make<myb::BinaryExpr>(@$,myb::ArithmeticOrCompare{myb::BinaryOp::Modulo},@2,std::move($1),std::move($3));};
unary: postfix {$$=std::move($1);}
 | PLUS unary {$$=driver.factory().make<myb::UnaryExpr>(@$,myb::UnaryOp::Plus,@1,std::move($2));}
 | MINUS unary {$$=driver.factory().make<myb::UnaryExpr>(@$,myb::UnaryOp::Minus,@1,std::move($2));}
 | NOT unary {$$=driver.factory().make<myb::UnaryExpr>(@$,myb::UnaryOp::Not,@1,std::move($2));};
postfix: primary {$$=std::move($1);}
 | postfix DOT IDENTIFIER {$$=driver.factory().make<myb::FieldExpr>(@$,std::move($1),std::move($3),@3,@2);}
 | postfix LBRACKET expression RBRACKET {$$=driver.factory().make<myb::IndexExpr>(@$,std::move($1),std::move($3),@2);}
 | postfix LBRACKET optional_expression COLON optional_expression RBRACKET {$$=driver.factory().make<myb::SliceExpr>(@$,std::move($1),std::move($3),std::move($5),@2);};
primary: INT {$$=driver.factory().make<myb::IntExpr>(@$,std::move($1.digits));}
 | FLOAT {$$=driver.factory().make<myb::FloatExpr>(@$,std::move($1.text));}
 | STRING {$$=driver.factory().make<myb::StringExpr>(@$,std::move($1));}
 | TRUE {$$=driver.factory().make<myb::BoolExpr>(@$,true);}
 | FALSE {$$=driver.factory().make<myb::BoolExpr>(@$,false);}
 | NULL_VALUE {$$=driver.factory().make<myb::NullExpr>(@$);}
 | IDENTIFIER {$$=driver.factory().make<myb::NameExpr>(@$,std::move($1),@1);}
 | LPAREN expression RPAREN {$$=driver.factory().make<myb::GroupExpr>(@$,std::move($2));}
 | IDENTIFIER LPAREN arguments RPAREN {$$=driver.factory().make<myb::CallExpr>(@$,std::move($1),@1,std::move($3));}
 | IDENTIFIER LBRACE fields RBRACE {$$=driver.factory().make<myb::ConstructExpr>(@$,std::move($1),@1,std::move($3));}
 | LBRACKET arguments RBRACKET {$$=driver.factory().make<myb::ArrayExpr>(@$,std::move($2));};
%%
// 首个语法错误直接传播，Bison 栈中的 variant 与 unique_ptr 随展开释放。
void myb::Parser::error(const location_type& where,const std::string& text){myb::fail({driver.failure(),where},myb::FailureDomain::Frontend,myb::ReasonCode::Syntax,text);}
