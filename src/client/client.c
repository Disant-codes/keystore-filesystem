#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "job_executor.h"

static struct option long_options[] = {
    {"connect", required_argument, 0, 'c'},
    {"put", required_argument, 0, 'p'},
    {"get", required_argument, 0, 'g'},
    {"delete", required_argument, 0, 'd'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --connect <IP Address>:<port>  Connect to server\n");
    fprintf(stderr, "  --put <key> <value>           Put key-value pair\n");
    fprintf(stderr, "  --get <key>                   Get value for key\n");
    fprintf(stderr, "  --delete <key>                Delete key\n");
    fprintf(stderr, "  --help                        Show this help message\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s --connect 127.0.0.1:8080 --put mykey myvalue\n", program_name);
    fprintf(stderr, "  %s --connect 127.0.0.1:8080 --get mykey\n", program_name);
}

int parse_and_validate(int argc, char **argv, char **server_ip, int *server_port, enum job_type *type, char **key, char **value) {
    int option_index = 0;
    int c;
    while ((c = getopt_long(argc, argv, "c:p:g:d:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c': // --connect
                *server_ip = strtok(optarg, ":");
                if (*server_ip) {
                    char *port_str = strtok(NULL, ":");
                    if (port_str) {
                        *server_port = atoi(port_str);
                    } else {
                        fprintf(stderr, "Error: Invalid format for --connect. Use IP:PORT\n");
                        return 1;
                    }
                } else {
                    fprintf(stderr, "Error: Invalid format for --connect. Use IP:PORT\n");
                    return 1;
                }
                break;

            case 'p': // --put
                *type = PUT;
                *key = optarg;
                if (optind < argc) {
                    *value = argv[optind];
                    optind++; // Move to next argument
                } else {
                    fprintf(stderr, "Error: --put requires both key and value\n");
                    return 1;
                }
                break;

            case 'g': // --get
                *type = GET;
                *key = optarg;
                break;

            case 'd': // --delete
                *type = DELETE;
                *key = optarg;
                break;

            case 'h': // --help
                print_usage(argv[0]);
                exit(0);

            case '?':
                // Invalid option
                return 1;

            default:
                abort();
        }
    }

    // Check if required options are provided
    if (!*server_ip || !*server_port || *type == INVALID_TYPE || !*key) {
        fprintf(stderr, "Error: Missing required options\n");
        fprintf(stderr, "Use --help for usage information\n");
        return 1;
    }

    // Validate key length
    if (*key && strlen(*key) > 128) {
        fprintf(stderr, "Error: Key length exceeds 128 characters\n");
        return 1;
    }

    // Additional check for PUT operation
    if (*type == PUT) {
        if (!*value) {
            fprintf(stderr, "Error: --put requires both key and value\n");
            return 1;
        }
        if (strlen(*value) > 1024) {
            fprintf(stderr, "Error: Value length exceeds 1024 characters\n");
            return 1;
        }
    }
    return 0; // Continue execution
}

int connect_to_server(const char *server_ip, int server_port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    return sock;
}

void print_job_response(const job_response *res) {
    printf("Job Response:\n");
    printf("  Type: %d\n", res->type);
    printf("  Status: %d\n", res->status);
    printf("  Error: %d\n", res->error);
    printf("  Data Length: %d\n", res->data_len);
    if (res->data && res->data_len > 0) {
        printf("  Data: %.*s\n", res->data_len, res->data);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    char *server_ip = NULL;
    int server_port = 0;
    enum job_type type = INVALID_TYPE;
    char *key = NULL;
    char *value = NULL;
    job_request *req = NULL;
    struct timespec delay;
    delay.tv_sec = 0; // Seconds
    delay.tv_nsec = 100000000; //100ms

    int parse_result = parse_and_validate(argc, argv, &server_ip, &server_port, &type, &key, &value);
    if (parse_result != 0) {
        if (parse_result > 0) {
            return 1; // error
        }
        return 0; // help printed
    }

    req = job_request_init(type, key, value);
    if (!req) return 1;
    
    int sock = connect_to_server(server_ip, server_port);
    if (sock < 0) {
        job_request_free(req);
        return 1;
    }
    
    // Send request
    if (send(sock, req, sizeof(job_request), 0) < 0) {
        perror("send");
        close(sock);
        job_request_free(req);
        return 1;
    }

    // Read multiple responses until job is completed or failed
    job_response *res = job_response_init(type);
    if (!res) {
        perror("Failed to allocate response");
        close(sock);
        job_request_free(req);
        return 1;
    }
    
    ssize_t n;
    int response_count = 0;
    
    printf("Waiting for job responses...\n");
    
    while (1) {
        // Clear the response structure for each new response
        memset(res, 0, sizeof(job_response));
        res->type = type;
        
        n = recv(sock, res, sizeof(job_response), 0);
        if (n < 0) {
            perror("recv");
            break;
        }
        if (n == 0) {
            printf("Server closed connection\n");
            break;
        }
        
        response_count++;
        printf("Received response %d:\n", response_count);
        print_job_response(res);
        
        // Check if job is completed or failed
        if (res->status == COMPLETED) {
            printf("Job completed successfully!\n");
            break;
        } else if (res->status == FAILED) {
            printf("Job failed!\n");
            break;
        } else if (res->status == PROCESSING) {
            printf("Job is still processing...\n");
        } else if (res->status == SUBMITTED) {
            printf("Job has been submitted...\n");
        }
        
        // Small delay to avoid busy waiting
        nanosleep(&delay, NULL); // 100ms
    }
    
    printf("Total responses received: %d\n", response_count);
    
    // Cleanup
    job_response_free(res);
    job_request_free(req);
    close(sock);
    return 0;
}