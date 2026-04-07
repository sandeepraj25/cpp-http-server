CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  :=

TARGET   := server
SRC      := src/main.cpp

.PHONY: all release debug clean run

all: release

release: CXXFLAGS += -O2 -DNDEBUG
release: $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)
	@echo "Built release: ./$(TARGET)"

debug: CXXFLAGS += -g -O0 -DDEBUG -fsanitize=address,undefined
debug: LDFLAGS  += -fsanitize=address,undefined
debug: $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET)_debug $(SRC) $(LDFLAGS)
	@echo "Built debug: ./$(TARGET)_debug"

run: release
	mkdir -p logs static
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET)_debug