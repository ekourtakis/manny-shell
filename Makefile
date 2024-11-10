make:
	gcc parse.c shell.c -o shell -Werror -Wall

run: 
	make; ./shell
	
clean:
	rm -f shell