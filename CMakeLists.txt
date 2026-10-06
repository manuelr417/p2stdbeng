cmake_minimum_required(VERSION 4.3)
project(p2stdbeng)

set(CMAKE_CXX_STANDARD 20)

set(CORE_SOURCES
    file/BlockFile.cpp
    bufpool/BufferPool.cpp bufpool/DataFrame.h bufpool/BufferPool.h
    policy/ReplacementPolicy.h policy/LRUPolicy.cpp policy/LRUPolicy.h
    bufpool/BufferManager.cpp bufpool/BufferManager.h
    heapfile/SlottedPage.cpp heapfile/SlottedPage.h
    heapfile/HeapFile.cpp heapfile/HeapFile.h
    tuple/Column.h tuple/Tuple.cpp tuple/Tuple.h
    schema/ColumnSchema.h schema/TableSchema.cpp schema/TableSchema.h
    schema/TupleSerializer.cpp schema/TupleSerializer.h
    schema/TableSchemaJson.cpp schema/TableSchemaJson.h
    schema/TableCsv.cpp schema/TableCsv.h
    schema/TableGenerator.cpp schema/TableGenerator.h
    sql/Token.h sql/Lexer.cpp sql/Lexer.h
    sql/Ast.h sql/Parser.cpp sql/Parser.h
    sql/AstPrinter.cpp sql/AstPrinter.h
    exec/BoundExpr.cpp exec/BoundExpr.h
    exec/Binder.cpp exec/Binder.h
    exec/Executor.cpp exec/Executor.h)

add_library(dbcore STATIC ${CORE_SOURCES})
target_include_directories(dbcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(p2stdbeng main.cpp)
target_link_libraries(p2stdbeng PRIVATE dbcore)

add_executable(p2stdbeng_demo p2stdbeng_demo.cpp)
target_link_libraries(p2stdbeng_demo PRIVATE dbcore)

enable_testing()
add_test(NAME p2stdbeng_checks COMMAND p2stdbeng)
set_tests_properties(p2stdbeng_checks PROPERTIES TIMEOUT 60)
