
ifeq (${MAKECMDGOALS}, debug)
	cflag = -g -DDEBUG
	dir = ./debug
else
	cflag = -O2 -DNDEBUG
	lflag = -s
	dir = ./release
endif

module = ssl_test
target = ${dir}/${module}
sources = ${shell ls *.cpp}
objects = ${patsubst %.cpp, ${dir}/%.o, ${sources}}
headers = ${shell ls *.h 2>/dev/null}
${shell mkdir -p ${dir}}


release debug : ${target}
${target} : ${objects}
	g++ -o ${target} ${objects} ${lflag} -pthread -lcrypto -lssl -lboost_system -lboost_thread
${objects} : ${dir}/%.o : %.cpp ${headers}
	g++ -Wall -fexceptions -std=c++11 ${cflag} -c $< -o $@


.PHONY : clean
clean:
	-rm -rf ./debug ./release

