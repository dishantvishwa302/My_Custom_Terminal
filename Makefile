CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
X11LIBS  := -lX11 -lutil

.PHONY: all clean

all: myterm myshell history hsearch multiWatch

myterm: myterm.cpp gui.cpp pty.cpp ansi.cpp gui.h pty.h ansi.h
	$(CXX) $(CXXFLAGS) -o myterm myterm.cpp gui.cpp pty.cpp ansi.cpp $(X11LIBS)

myshell: myshell.cpp histpath.h
	$(CXX) $(CXXFLAGS) -o myshell myshell.cpp

history: history.cpp histpath.h
	$(CXX) $(CXXFLAGS) -o history history.cpp

hsearch: hsearch.cpp histpath.h
	$(CXX) $(CXXFLAGS) -o hsearch hsearch.cpp

multiWatch: multiWatch.cpp
	$(CXX) $(CXXFLAGS) -o multiWatch multiWatch.cpp

clean:
	rm -f myterm myshell history hsearch multiWatch
