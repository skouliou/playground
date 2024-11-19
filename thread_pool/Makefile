FLAGS:=-g -DLOG_USE_COLOR

test: thread_pool.c log.c
	gcc thread_pool.c log.c $(FLAGS) -o test

clean:
	-@rm test
