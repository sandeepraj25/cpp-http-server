CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -pthread
LDFLAGS  := -pthread

TARGET   := server
SRC      := src/main.cpp
HEADERS  := $(wildcard include/*.h)
TEST_SRC := tests/unit_tests.cpp

.PHONY: all release debug asan tsan unit unit-tsan integration test sanitize bench run clean

all: release

# Optimized build used for running and benchmarking
release: $(TARGET)
$(TARGET): $(SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -O2 -DNDEBUG -o $@ $(SRC) $(LDFLAGS)
	@echo "Built release: ./$(TARGET)"

# AddressSanitizer + UndefinedBehaviorSanitizer: memory bugs, overflows, UB
asan debug: $(SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
		-o $(TARGET)_asan $(SRC) $(LDFLAGS) -fsanitize=address,undefined
	@echo "Built: ./$(TARGET)_asan"

# ThreadSanitizer: data races between worker threads
tsan: $(SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -g -O1 -fsanitize=thread -o $(TARGET)_tsan $(SRC) $(LDFLAGS) -fsanitize=thread
	@echo "Built: ./$(TARGET)_tsan"

# Unit tests (parser, router, rate limiter, thread pool, middleware)
unit: $(TEST_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -g -O1 -fsanitize=address,undefined -o unit_tests $(TEST_SRC) $(LDFLAGS) -fsanitize=address,undefined
	./unit_tests

# End-to-end tests against the real server
integration: release
	SERVER=./$(TARGET) bash tests/integration_test.sh

test: unit integration

# Run the integration suite under each sanitizer
# and fail if any sanitizer printed a report (even one found during shutdown)
SANITIZER_REPORT := "AddressSanitizer|ThreadSanitizer|LeakSanitizer|runtime error"
sanitize: asan tsan unit-tsan
	SERVER=./$(TARGET)_asan PORT=18081 bash tests/integration_test.sh
	@! grep -E $(SANITIZER_REPORT) logs/integration_server.log
	SERVER=./$(TARGET)_tsan PORT=18082 bash tests/integration_test.sh
	@! grep -E $(SANITIZER_REPORT) logs/integration_server.log
	@echo "Sanitizers: no errors, no data races"

# Unit tests under ThreadSanitizer (thread pool + rate limiter concurrency)
unit-tsan: $(TEST_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -g -O1 -fsanitize=thread -o unit_tests_tsan $(TEST_SRC) $(LDFLAGS) -fsanitize=thread
	TSAN_OPTIONS=halt_on_error=1 ./unit_tests_tsan

# Load test with wrk (https://github.com/wg/wrk)
bench: release
	bash bench/run_benchmarks.sh

run: release
	mkdir -p logs static
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET)_asan $(TARGET)_tsan $(TARGET)_debug unit_tests unit_tests_tsan
