CFLAGS ?= -O2 -Wall -Wextra
test_weather: test_weather.c ../app/weather.c ../app/weather.h
	$(CC) $(CFLAGS) test_weather.c ../app/weather.c -lm -o $@

check: test_weather
	./test_weather

clean:
	rm -f test_weather
.PHONY: check clean
