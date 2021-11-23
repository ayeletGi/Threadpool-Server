#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>		// for read/write/close
#include <sys/types.h>  /* standard system types        */
#include <netinet/in.h> /* Internet address structures */
#include <sys/socket.h> /* socket interface functions  */
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <signal.h>
#include <fcntl.h>

#include "threadpool.h"

#define SYS_CALL_FAILED -1
#define NO_PERMISSION -2
#define SUCCESS 0

#define MAX_REQUEST 4000
#define FILE_LINE 500
#define FILE_NAME 500

#define HEADERS_LENGTH 600
#define DATE_LENGTH 128
#define SMALL_HEADER 200

#define ERROR_TEMPLATE "HTTP/1.1 %d %s\r\nServer: webserver/1.0\r\nDate: %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
#define FOUND_TEMPLATE "HTTP/1.1 %d %s\r\nServer: webserver/1.0\r\nDate: %s\r\nLocation: %s/\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
#define OK_TEMPLATE "HTTP/1.1 %d %s\r\nServer: webserver/1.0\r\nDate: %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nLast-Modified: %s\r\nConnection: close\r\n\r\n"
#define OK_NO_TYPE "HTTP/1.1 %d %s\r\nServer: webserver/1.0\r\nDate: %s\r\nContent-Length: %d\r\nLast-Modified: %s\r\nConnection: close\r\n\r\n"

#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"

#define BAD_REQUEST "<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\r\n<BODY><H4>400 Bad request</H4>\r\nBad Request.\r\n</BODY></HTML>\r\n" 
#define NOT_FOUND "<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\r\n<BODY><H4>404 Not Found</H4>\r\nFile not found.\r\n</BODY></HTML>\r\n"
#define NOT_SUPPORTED "<HTML><HEAD><TITLE>501 Not supported</TITLE></HEAD>\r\n<BODY><H4>501 Not supported</H4>\r\nMethod is not supported.\r\n</BODY></HTML>\r\n"
#define FORBIDDEN "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\r\n<BODY><H4>403 Forbidden</H4>\r\nAccess denied.\r\n</BODY></HTML>\r\n"
#define FOUND "<HTML><HEAD><TITLE>302 Found</TITLE></HEAD>\r\n<BODY><H4>302 Found</H4>\r\nDirectories must end with a slash.\r\n</BODY></HTML>\r\n"
#define SERVER_ERROR "<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\r\n<BODY><H4>500 Internal Server Error</H4>\r\nSome server side error.\r\n</BODY></HTML>\r\n"
#define SERVER_ERROR_LENGTH 144

#define DIR_START "<HTML>\r\n<HEAD><TITLE>Index of %s</TITLE></HEAD>\r\n\r\n<BODY>\r\n<H4>Index of %s</H4>\r\n\r\n<table CELLSPACING=8>\r\n<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\r\n\r\n"
#define DIR_FILE "<tr>\r\n<td><A HREF=\"%s\">%s</A></td><td></td><td></td>\r\n</tr>\r\n\r\n"
#define R_DIR_FILE "<tr>\r\n<td><A HREF=\"%s\">%s</A></td><td>%s</td><td>%d</td>\r\n</tr>\r\n\r\n"
#define DIR_END "</table>\r\n\r\n<HR>\r\n\r\n<ADDRESS>webserver/1.0</ADDRESS>\r\n\r\n</BODY></HTML>\r\n"

/*
Name: Ayelet Gibli
Id: 208691675
*/

/************* Declartions *************/
struct response
{
	int version;
	char status[SMALL_HEADER];
	char date[DATE_LENGTH+1];
	
	int location;
	char* path;

	int c_t;
	char content_type[SMALL_HEADER];
	
	int l_m;
	char last_modified[DATE_LENGTH+1];

	char *headers;
	int headers_length;

	char* html_content;
	unsigned char* file_content;
	int content_length; 


} typedef response;

void initlizeStruct(response* res);
void freeStruct(response* res);
void sysCallError(char *str, threadpool* pool);
void userError();
void internalServerError500(int client_fd, response* res);
char *get_mime_type(char *name);
int checkPermission(response* res);
int badRequest400(response* res);
int notFound404(response *res);
int notSupported501(response *res);
int forbiddenResponse403(response *res);
int found302(response *res);
void ok200(response *res);
int fileResponse(response *res);
int dirContent(response* res);
int checkDirectory(response *res);
int buildHeaders(response *res);
int checkRequest(response *res, char *request);
int handleClient(void *client_fd);

/************* main *************/
int main(int argc, char *argv[])
{
	/* check user input */
	if (argc != 4)
		userError();

	int port = atoi(argv[1]);
	int pool_size = atoi(argv[2]);
	int num_of_request = atoi(argv[3]);

	if (port <= 0 || pool_size <= 0 || num_of_request <= 0 || pool_size > MAXT_IN_POOL)
		userError();

	/* Initilize the  threadpool.*/
	threadpool* pool = create_threadpool(pool_size);
	if(pool == NULL){
		sysCallError("create_threadpool failed.", NULL);
	}

	/* Initilize the server.*/
	int srv_fd;
	struct sockaddr_in srv;
	int check;

	/* Socket */
	srv_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (srv_fd < 0)
		sysCallError("socket failed.",pool);

	/* Bind */
	srv.sin_family = AF_INET;
	srv.sin_port = htons(port);
	srv.sin_addr.s_addr = htonl(INADDR_ANY);
	check = bind(srv_fd, (struct sockaddr *)&srv, sizeof(srv));
	if (check < 0)
		sysCallError("bind failed.",pool);

	/* Listen */
	check = listen(srv_fd, num_of_request);
	if (check < 0)
		sysCallError("listen failed.",pool);

	/* Accept */
	int *c_sockets = (int *)malloc(num_of_request * sizeof(int));
	if (c_sockets == NULL)
		sysCallError("malloc failed.",pool);

	struct sockaddr cli_addr;
	socklen_t clilen = sizeof(cli_addr);

	for (int i = 0; i < num_of_request; i++)
	{
		c_sockets[i] = accept(srv_fd, &cli_addr, &clilen);
		if (c_sockets[i] < 0)
			sysCallError("accept failed",pool);
		
		dispatch(pool, &handleClient,&(c_sockets[i]));
	}

	close(srv_fd);
	destroy_threadpool(pool);
	free(c_sockets);

	return EXIT_SUCCESS;
}

//*******Functions to manage the struct *******//
/**
 * This function init struct response. 
 * And init the curr time, and put "text/html" as content type default.
 */
void initlizeStruct(response* res)
{ 
	memset(res->status,'\0',SMALL_HEADER);
	memset(res->date,'\0',DATE_LENGTH+1);
	memset(res->content_type,'\0',SMALL_HEADER);
	memset(res->last_modified,'\0',DATE_LENGTH+1);

	time_t now;
	now = time(NULL);
	strftime(res->date, DATE_LENGTH, RFC1123FMT ,gmtime(&now));	//timebuf holds the correct format of the current time.
	
	strcpy(res->content_type, "text/html"); //default
	
	res->version = 0;
	res->location = 0;
	res->path = NULL;
	res->l_m = 0;
	res->c_t = 1;
	res->headers = NULL ;
	res->headers_length = 0;
	res->html_content = NULL;
	res->file_content = NULL;
	res->content_length = 0;
}
/**
 * This function free struct response. 
 */
void freeStruct(response* res)
{
	if(res->headers != NULL)
		free(res->headers);
	if(res->html_content != NULL)
		free(res->html_content);
	if(res->file_content != NULL)
		free(res->file_content);
	if(res->path != NULL)
		free(res->path);
	free(res);
}
//*******Functions to print errors *******//
/**
 * This function handle some system call error.
 * Prints the sys call that failed and exit the program. 
 */
void sysCallError(char *str, threadpool* pool)
{
	perror(str);
	perror("\n");
	if(pool != NULL)
		destroy_threadpool(pool);
	exit(EXIT_FAILURE);
}
/**
 * This function handle some usage error.
 * Prints the right usage msg and exit the program. 
 */
void userError()
{
	fprintf(stderr, "Usage: server <port> <pool-size> <max-number-of-request>\n");
	exit(EXIT_FAILURE);
}
/**
 * This function handle errors that happend while handaling the client.
 * Sends "500 - Internal server error" msg to the client and exit the thread. 
 */
void internalServerError500(int client_fd, response* res)
{
	//if res malloc didn't fail
	if(res != NULL)
		freeStruct(res);

	char error[HEADERS_LENGTH + SERVER_ERROR_LENGTH];
	char date[DATE_LENGTH];
	memset(error,'\0',HEADERS_LENGTH + SERVER_ERROR_LENGTH);
	memset(date,'\0',DATE_LENGTH);

	time_t now;
	now = time(NULL);
	strftime(date, DATE_LENGTH, RFC1123FMT ,gmtime(&now));	//timebuf holds the correct format of the current time.

	int n = sprintf(error, ERROR_TEMPLATE, 500, "Internal Server Error", date, "text/html", 137);
	strcpy(error+n,SERVER_ERROR);
	
	//sends the error message
	write(client_fd, error, strlen(error));
	close(client_fd);
	return;
}
//******* other helping functions *******//
/**
 * This function gets a file path and returns the file type.
 * If it cannot recognize the type, returns NULL.
 */
char *get_mime_type(char *name)
{
	char *ext = strrchr(name, '.');
	if (!ext)
		return NULL;
	if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
		return "text/html";
	if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
		return "image/jpeg";
	if (strcmp(ext, ".gif") == 0)
		return "image/gif";
	if (strcmp(ext, ".png") == 0)
		return "image/png";
	if (strcmp(ext, ".css") == 0)
		return "text/css";
	if (strcmp(ext, ".au") == 0)
		return "audio/basic";
	if (strcmp(ext, ".wav") == 0)
		return "audio/wav";
	if (strcmp(ext, ".avi") == 0)
		return "video/x-msvideo";
	if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0)
		return "video/mpeg";
	if (strcmp(ext, ".mp3") == 0)
		return "audio/mpeg";
	return NULL;
}
/**
 * This function gets a file path and returns "SUCCESS" if it has the right perrmision.
 * else - returns "NO_PERMISSION".
 * Checks "X -> others" permission for all the directories in the path, and "R -> others" for the file itself.
 */
int checkPermission(response* res)
{
	/* get the first directory */
   char* dir;
   struct stat fs;
   int index = 2; //for './' at the start.

   char* curr_path = (char*) malloc(sizeof(char) * (strlen(res->path)+1));
   if(curr_path == NULL){
		perror("malloc.");
		return SYS_CALL_FAILED;
   }
   memset(curr_path, '\0',strlen(res->path)+1);

   //creates a copy of the path.
   char* path_copy = (char*) malloc(sizeof(char) * (strlen(res->path)+1));
    if(path_copy == NULL){
		free(curr_path);
		perror("malloc.");
		return SYS_CALL_FAILED;
   }
   memset(path_copy, '\0',strlen(res->path)+1);

   strcpy(path_copy, res->path+1);

	/* make sure to not check the last one. */
	if(path_copy[strlen(path_copy)-1] == '/')
		path_copy[strlen(path_copy)-1] = '\0';

	char* tmp  = strrchr(path_copy, '/');
	if(tmp != NULL){
		tmp[0] = '\0';
	}

   /* loop through all the directories in the path  */
   dir = strtok(path_copy, "/");

   while(dir != NULL)
   {  
		index += strlen(dir);  //index = num of chars until the curr '/'.
		index ++; //include this '/'.

		strncpy(curr_path, res->path, index); //curr_path = path to the curr directory to check.
	    curr_path[index] = '\0';
			   	
		if(stat(curr_path, &fs) < 0 ){
			free(curr_path);
			free(path_copy);
			return NO_PERMISSION;
		}
		if(!(S_IXOTH & fs.st_mode)){ //no executing permission.
			free(curr_path);
			free(path_copy);
			return NO_PERMISSION;
		}
		dir = strtok(NULL,  "/");
   
   }

	free(curr_path);
	free(path_copy);

	/* now check the end file*/
	if(stat(res->path, &fs) < 0 )
		return NO_PERMISSION;
	if(!(S_IROTH & fs.st_mode)) //no reading permission
		return NO_PERMISSION;
	
	return SUCCESS;
}
//******* Functions the build headers + error content *******//
/**
 * This function fills in the struct the "400 - Bad request" headers values.
 * Its update the response content with the right msg.
 */
int badRequest400(response* res)
{
	res->version = 400;
	strcpy(res->status, "Bad Request");

	res->content_length = strlen(BAD_REQUEST);
	res->html_content = (char*)malloc(sizeof(char) * (res->content_length+1));
	if(res->html_content == NULL){
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}
	memset(res->html_content,'\0',res->content_length+1);
	strcpy(res->html_content, BAD_REQUEST);
	
	return SUCCESS;
}
/**
 * This function fills in the struct the "404 - Not found" headers values.
 * Its update the response content with the right msg.
 */
int notFound404(response *res)
{
	res->version = 404;
	strcpy(res->status, "Not Found");

	res->content_length = strlen(NOT_FOUND);
	res->html_content = (char *)malloc(sizeof(char) * (res->content_length+1));
	if(res->html_content == NULL){
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}
	memset(res->html_content,'\0',res->content_length+1);
	strcpy(res->html_content, NOT_FOUND);

	return SUCCESS;
}
/**
 * This function fills in the struct the "501 - Not supported" headers values.
 * Its update the response content with the right msg.
 */
int notSupported501(response *res)
{
	res->version = 501;
	strcpy(res->status, "Not supported");

	res->content_length = strlen(NOT_SUPPORTED);
	res->html_content = (char *)malloc(sizeof(char) * (res->content_length+1));
	if(res->html_content == NULL){
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}
	memset(res->html_content,'\0',res->content_length+1);
	strcpy(res->html_content, NOT_SUPPORTED);

	return SUCCESS;
}
/**
 * This function fills in the struct the "403 - Forbidden" headers values.
 * Its update the response content with the right msg.
 */
int forbiddenResponse403(response *res)
{
	res->version = 403;
	strcpy(res->status, "Forbidden");

	res->content_length = strlen(FORBIDDEN);
	res->html_content = (char *)malloc(sizeof(char) * (res->content_length+1));
	if(res->html_content == NULL){
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}
	memset(res->html_content,'\0',res->content_length+1);
	strcpy(res->html_content,FORBIDDEN);
	
	return SUCCESS;
}
/**
 * This function fills in the struct the "302 - Found" headers values.
 * Its update the response content with the right msg.
 */
int found302(response *res)
{
	res->version = 302;
	strcpy(res->status, "Found");
	res->location = 1;

	res->content_length = strlen(FOUND);
	res->html_content = (char *)malloc(sizeof(char) * (res->content_length+1));
	if(res->html_content == NULL){
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}
	memset(res->html_content,'\0',res->content_length+1);
	strcpy(res->html_content,FOUND);
	return SUCCESS;
}
/**
 * This function fills in the struct the "200 - OK" headers values.
 */
void ok200(response *res)
{
	res->version = 200;
	strcpy(res->status, "OK");
	res->l_m = 1;
}
//******* Functions the build specific response content *******//
/**
 * This function prepare sending a specific file to the client.
 * Update the "200 - OK" headers using ok200 func.
 * Updates the "Content_Type" and "Content_length" header.
 * Reading the file into the struct "file_content".
 */
int fileResponse(response *res)
{
	/* fills the response headers */
	ok200(res);
	char * tmp = get_mime_type(res->path); //checks file type.
	if(tmp != NULL)
		strcpy(res->content_type, tmp);
	else
		res->c_t = 0; // dont print the "content type" header.
	
	/* prepare stuff. */
	int length, rc;
	struct stat fs;

	stat(res->path, &fs); //get the file size.
	length = fs.st_size;
	res->content_length = length;

	res->file_content = (unsigned char*)malloc(sizeof(unsigned char) * (length+1));
	if(res->file_content == NULL){
		perror("malloc.");
		return SYS_CALL_FAILED;
	}

	int fd = open(res->path, O_RDONLY, 0);
	if(fd < 0){
		perror("open.");
		return SYS_CALL_FAILED;
	} 

	/* copy the file into content arr. */
	rc = read(fd, res->file_content, length);
	if(rc < 0){
		perror("read.");
		return SYS_CALL_FAILED;
	}
	res->file_content[length] = '\0';
	close(fd);
	return SUCCESS;
}
/**
 * This function prepare sending a specific dir content (In the defiened html form) to the client.
 * Update the "200 - OK" headers using ok200 func.
 * Updates the "Content_length" header.
 * Update the response content with the dir table: reading the names of all the files in the dir.
 * For each file, writes its own line with: "<link to the file>, <file name>, <last modified>, <file size>".
 */
int dirContent(response* res)
{
	/* fills the response headers */
	ok200(res);
	
	/* opens dir */
	DIR* dir = opendir(res->path);
	if(dir == NULL){
		perror("opendir failed.");
		return SYS_CALL_FAILED;
	}
	
	/* some helping varaibles */
	int curr_length = 0, n;
	struct dirent* dentry;
	struct stat fs;
	
	char* file_path = (char*)malloc(sizeof(char)* (strlen(res->path) + FILE_NAME + 1 )); // for each file keeps it own path for "stat".
	if(file_path == NULL){
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}
	strcpy(file_path, res->path);

	/* fills the first part of the content */
	res->html_content = (char*) malloc(sizeof(char) * (strlen(DIR_START)+ 2*strlen(res->path) +1));
	if(res->html_content == NULL){
		free(file_path);
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}

	n = sprintf(res->html_content, DIR_START, res->path, res->path);
	curr_length += n;

	/* loop all files in the directory */
	while((dentry = readdir(dir)) != NULL)
	{
		strcpy(file_path + strlen(res->path), dentry->d_name);	

		//adds more space for this file table entery
		res->html_content = (char*) realloc(res->html_content, sizeof(char) * (curr_length + FILE_LINE + 1 ));
		if(res->html_content == NULL){
			free(file_path);
			perror("realloc failed.");
			return SYS_CALL_FAILED;
		}
		//get this file information.
		if(stat(file_path, &fs) < 0 )
			n = sprintf(res->html_content + curr_length, DIR_FILE, dentry->d_name, dentry->d_name);
		
		else{
			char timebuf[DATE_LENGTH+1];
			time_t now = fs.st_mtime; //get the file last modifaction date
			strftime(timebuf, DATE_LENGTH, RFC1123FMT ,gmtime(&now));;	

			/* fills this file part of the content */
			n = sprintf(res->html_content + curr_length, R_DIR_FILE, dentry->d_name, dentry->d_name, timebuf, (int)fs.st_size);
		}
		curr_length += n;
		
	}
	free(file_path);

	/* fills the end part of the content */
	res->html_content = (char*) realloc(res->html_content, sizeof(char) * (curr_length + strlen(DIR_END) + 1));
	if(res->html_content == NULL){
		perror("realloc failed.");
		return SYS_CALL_FAILED;
	}

	strcat(res->html_content, DIR_END);
	curr_length += strlen(DIR_END);
	res->content_length = curr_length;

	/*finished */
	if(closedir(dir) < 0){
		perror("closedir failed.");
		return SYS_CALL_FAILED;
	}

	return SUCCESS;
}
//*******Functions to pharse a client request *******//
/**
 * This function get a request path to some directory.
 * Checks if the dir has the "index.html" file.
 * Yes -> return it by calling fileResponse func with updated path.
 * No -> return diContent func.
 */
int checkDirectory(response *res)
{
	/* first - searching for index.html */
	int len = strlen(res->path); 	// update path += index.html
	res->path = (char*)realloc(res->path, (len + 12) * sizeof(char));
	if(res->path == NULL){
		perror("realloc failed.");
		return SYS_CALL_FAILED;
	}
	strcat(res->path, "index.html");
	res->path[len + 11] = '\0';

	/* checks for index.html */
	struct stat fs;
	int s = stat(res->path, &fs);
	if(s < 0)
	{
		/* index.html was not found*/
		res->path[len] = '\0';
		return dirContent(res); 
	}

	/* index.html was found*/
	if(checkPermission(res) == NO_PERMISSION) //connot read index.html
		return forbiddenResponse403(res); 
	
	if(checkPermission(res) == SYS_CALL_FAILED) //some error happened in checkPermission
		return SYS_CALL_FAILED; 
	
	else
		return fileResponse(res); 
}
/**
 * This function build the final response headers string by the struct values.
 */
int buildHeaders(response *res)
{
	int n = 0;
	res->headers = (char *)malloc(sizeof(char) * (HEADERS_LENGTH+1));
	if(res->headers == NULL){
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}
	memset(res->headers, '\0',HEADERS_LENGTH+1);

	if(res->l_m == 1) //print last modified header?
	{ 
		if(res->c_t == 1) //print content type header?
			n = sprintf(res->headers, OK_TEMPLATE, res->version, res->status, res->date, res->content_type, res->content_length, res->last_modified);
		else
			n = sprintf(res->headers, OK_NO_TYPE, res->version, res->status, res->date, res->content_length, res->last_modified);
	}
	else if(res->location == 1) //print location header?
		n = sprintf(res->headers, FOUND_TEMPLATE, res->version, res->status, res->date, res->path+1, res->content_type, res->content_length);
	else
		n = sprintf(res->headers, ERROR_TEMPLATE, res->version, res->status, res->date,res->content_type, res->content_length);

	res->headers[n] = '\0';
	res->headers_length = n;

	return SUCCESS;
}
/**
 * This function check the client request.
 * 1. Check the input.
 * 2. for each case build the response with the right helping func.
 */
int checkRequest(response *res, char *request)
{
	/* cut up to the first /r/n */
	char *tmp = strstr(request, "\r\n");
	if (tmp == NULL)
		return badRequest400(res);
	tmp[0] = ' ';
	tmp[1] = '\0';

	/* checks that there are 3 tokens in the request */
	//1st = method
	char* method = strtok(request, " ");
	if (method == NULL)
		return badRequest400(res);

	//2nd = path
	char* path = strtok(NULL, " ");
	if (path == NULL)
		return badRequest400(res);

	res->path = (char*)malloc(sizeof(char) * (strlen(path) + 2));
	if(res->path == NULL){
		perror("malloc failed.");
		return SYS_CALL_FAILED;
	}
	memset(res->path,'\0',strlen(path) + 2);

	res->path[0] = '.'; //add '.' at the start of the path.
	strcpy(res->path+1, path);	
	
	//3rd = protocol
	char *protocol = strtok(NULL, " ");
	if (protocol == NULL)
		return badRequest400(res);

	/* checks if the protocol is valid */
	if ((strcmp(protocol, "HTTP/1.0") != 0) && (strcmp(protocol,"HTTP/1.1") != 0))
		return badRequest400(res);

	/* checks if the method is valid */
	if (strcmp(method, "GET") != 0)
		return notSupported501(res);

	/* checks if path doesnt contain "\\" */
	if(strstr(res->path, "//") != NULL)
		return badRequest400(res);

	/* checks if the path exists */
	struct stat fs;
	int s = stat(res->path, &fs);
	if(s < 0)
		return notFound404(res);

	/* yes - path exists. */

	//get the file last modifaction date
	char timebuf[DATE_LENGTH+1];
	memset(timebuf,'\0',DATE_LENGTH+1);
	time_t now = fs.st_mtime; 
	strftime(timebuf, DATE_LENGTH, RFC1123FMT ,gmtime(&now));;	
	strcpy(res->last_modified, timebuf);

	/* checks if the path is for a directory */
	if(S_ISDIR(fs.st_mode))
	{		
		// doesn't end with '/'
		if(res->path[strlen(res->path) -1] != '/')
			return found302(res);
		// doesn't have permission
		if(checkPermission(res) == NO_PERMISSION)
			return forbiddenResponse403(res);
 		//some error happened in checkPermission
		if(checkPermission(res) == SYS_CALL_FAILED)
			return SYS_CALL_FAILED; 
		//ok. handle directory sepratly.
		else
			return checkDirectory(res);
	}

	/* checks if the path is for a regular file */
	if(S_ISREG(fs.st_mode))
	{
		// doesn't have permission
		if(checkPermission(res) == NO_PERMISSION)
			return forbiddenResponse403(res);
 		//some error happened in checkPermission
		if(checkPermission(res) == SYS_CALL_FAILED)
			return SYS_CALL_FAILED; 
		//ok. handle sending the file sepratly.
		else
			return fileResponse(res);
	}
	/* not a regular file */
	return forbiddenResponse403(res);
}
/**
 * This function is the main one that handle a client.
 * 1. Reads the client request.
 * 2. Calls checkRequest that build the response with the right headers and content.
 * 3. Writes the response headers to the client.
 * 4. Writes the response content to the client.
 */
int handleClient(void *client_fd)
{
	/* initlize */
	int socket = *((int*)client_fd);
	int rc;

	char request[MAX_REQUEST];
	memset(request,'\0',MAX_REQUEST);

	response *res = (response *)malloc(sizeof(response));
	if (res == NULL){
		perror("malloc failed.");
		internalServerError500(socket, res);
		return SYS_CALL_FAILED;
	}

	initlizeStruct(res);

	/* get the first line of the client request */
	rc = read(socket, request, MAX_REQUEST);
	if (rc < 0){
		perror("read failed.");
		internalServerError500(socket, res);
		return SYS_CALL_FAILED;
	}
	
	/* check the request and updates response feild */
	rc = checkRequest(res, request);
	if(rc == SYS_CALL_FAILED){
		internalServerError500(socket, res);
		return SYS_CALL_FAILED;
	}
	/* build the response headers */
	rc = buildHeaders(res);
	if(rc == SYS_CALL_FAILED){
		internalServerError500(socket, res);
		return SYS_CALL_FAILED;
	}
	signal(SIGPIPE, SIG_IGN);

	/* sends  the response headers */
	rc = write(socket, res->headers, strlen(res->headers)); 
	// if(rc < 0){
	// 	perror("write failed.");
	// 	freeStruct(res);
	// 	return SYS_CALL_FAILED;
	// }
	/* sends  the content */
	if(res->html_content != NULL){ 
		//send html content
		rc = write(socket, res->html_content, strlen(res->html_content));
		// if(rc < 0){
		// 	perror("write failed.");
		// 	freeStruct(res);
		// 	return SYS_CALL_FAILED;
		// }
	}
	else if(res->file_content != NULL){ 
		//send file content
		rc = write(socket, res->file_content, res->content_length);
		// if(rc < 0){
		// 	perror("write failed.");
		// 	freeStruct(res);
		// 	return SYS_CALL_FAILED;
		// }
	}
	close(socket);
	freeStruct(res);

	return 0;
}

