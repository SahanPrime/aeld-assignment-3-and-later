#include <fcntl.h>
#include <syslog.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

int main(int argc,char *argv[]){

openlog(NULL,LOG_PID,LOG_USER);

if (argc != 3){
	syslog(LOG_ERR,"Number of Arguements missing.Expected 2,got %d",argc-1);
	exit(1);
}

const char *writefile = argv[1];
const char *writestr = argv[2];

int fd;
ssize_t nr;

fd = open (writefile,O_RDWR|O_APPEND|O_CREAT,S_IRWXU);
if (fd == -1){
	syslog(LOG_ERR,"Failed to open file : %s",strerror(errno));
	exit(1);
}

const char *buf = writestr;

/*write the string in 'buf' to 'fd'*/
nr=write (fd,buf,strlen(buf));
if (nr ==-1){
	/*error*/
	syslog(LOG_ERR,"Failed to write string:%s",strerror(errno));
	exit(1);
}
else if (nr != strlen(buf)){
	syslog(LOG_ERR,"Partial Write");
}


/*close the file */
if (close (fd) == -1){
	syslog(LOG_ERR,"Failed to close the file : %s",strerror(errno));
	exit(1);
}

syslog(LOG_DEBUG,"Writing %s to %s",writestr,writefile);

return 0;
}

