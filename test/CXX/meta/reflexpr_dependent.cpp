// RUN: %clang -freflection -std=c++1z %s

#include <experimental/meta>

using namespace std::experimental;

// Check dependent reflexpr and instantiation

template<typename T>
struct S1 {
  constexpr T foo() const { return T(); }
};

template<typename T>
constexpr int test() {
  constexpr meta::info x1 = reflexpr(T);
  
  constexpr T* y1 = nullptr;
  constexpr meta::info x2 = reflexpr(y1);

  constexpr T y2 = T();
  constexpr meta::info x3 = reflexpr(y2);

  constexpr const T y3 = T();
  constexpr meta::info x4 = reflexpr(y3);

  constexpr S1<T> y4;
  constexpr meta::info x5 = reflexpr(y4);

  constexpr meta::info x6 = reflexpr(S1<T>::foo); // ERROR:foo is undeclared (see Sema::ActOnReflectedId)
  

  // Generate output
  (void)__reflect_print(x1);
  (void)__reflect_print(x2);
  (void)__reflect_print(x3);
  (void)__reflect_print(x4);
  (void)__reflect_print(x5);
  (void)__reflect_print(x6);
  return 0;
}

struct S { };

void test_templates() {
  constexpr int x1 = test<int>();
  constexpr int x2 = test<S>();
}

int main(int argc, char* argv[])
{
  test_templates();
}
