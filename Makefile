CXXFLAGS= -Wall -Wextra -Werror -std=c++23 -ggdb2 -I./include
CXX_SOURCES=flip_linux.cpp $(addprefix driver/, tap.cpp) $(addprefix flip/, protocol.cpp router.cpp)
OBJS= $(CXX_SOURCES:.cpp=.o)
all: flip_linux

flip_linux: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(OBJS) flip_linux