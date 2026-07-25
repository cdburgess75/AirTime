#include "test_framework.h"

int main() {
  std::printf("AirTime core unit tests\n\n");
  return airtime_test::run_all();
}
