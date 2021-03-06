#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/time.h>
#include "pipesort.h"

// global thread attributes
pthread_attr_t attr;
pthread_t thread;
// size of bounded buffer between threads
int buffer_size = 1;
int verbose = 0;
sem_t sorted;

/**
 *
 * usage: ./pipesort
 *
 * arguments:
 *      -v                      print numbers in output thread to standard out
 *      -s                      seed for rand()
 *      -l {number of elements} count of numbers to sort. default 100
 *      -b {size of buffer}     buffer size. default 1
 */
int main(int argc, char **argv) {

    int c;
    int seed = 42;
    int length = 100;
    struct timeval tv1, tv2;

    while((c = getopt(argc, argv, "l:s:b:v")) != -1) {
        switch(c) {
            case 'l':
                length = atoi(optarg);
                break;
            case 's':
                seed = atoi(optarg);
                break;
            case 'b':
                buffer_size = atoi(optarg);
                break;
            case 'v':
                verbose = 1;
                break;
            case '?':
                if(optopt == 'l' || optopt == 's' || optopt == 'b') {
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                }
                else if(isprint(optopt)) {
                    fprintf(stderr, "Unknown option '-%c'.\n", optopt);
                }
                else {
                    fprintf(stderr, "Unknown option character '\\x%x'.\n", optopt);
                }
                return -1;
            default:
                return -1;
        }
    }

    // Seed so that we can always reproduce the same (pseudo) random numbers
    srand(seed);

    // init semaphore so that main thread can wait on it
    sem_init(&sorted, 0, 0);

    // initialize thread attributes
    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    // prepare generator thread parameters
    Generator_params params;
    params.length = length;

    gettimeofday(&tv1, NULL);
    // start pipe sort
    pthread_create(&thread, &attr, generator, &params);

    // wait until sorting is finished
    sem_wait(&sorted);

    gettimeofday(&tv2, NULL);
    printf("Parameters: -b %i -l %i -s %i\n", buffer_size, length, seed);

    // calculate time needed
    double time = (double) (tv2.tv_usec - tv1.tv_usec) / 1000000 +
              (double) (tv2.tv_sec - tv1.tv_sec);
    printf("Time: %.6e\n", time);
}

/**
 * Routine for an output thread.
 * Receives numbers and prints them to standard out.
 *
 * @param p void pointer to a Comparator_params struct
 */
void *output(void *p) {
    // read buffer and semaphores
    Comparator_params *params = (Comparator_params*) p;
    sem_t *read_full = params->full;
    sem_t *read_empty = params->empty;
    int *read_buffer = params->buffer;
    int read_out = 0;

    int read_val;
    State state = INITIAL;
    int received = 0;
    int correct = 1;
    int prev_read_val = -1;

    printf("\nOutput: \n");


    /**
     * 2 different states for an output thread
     *
     * 1. INITIAL
     *
     * 2. END
     */
    while(1) {
        sem_wait(read_full);
        read_val = read_buffer[read_out];
        read_out = (read_out + 1) % buffer_size;
        sem_post(read_empty);

        if(read_val == -1) {
            // first END received
            if(state == INITIAL) {
                state = END;
                continue;
            } else {
                // second END received
                break;
            }
        }

        // print to console if verbose
        if(verbose) {
            printf("%i\n", read_val);
        }
        received++;
        if(prev_read_val > read_val) {
            correct = 0;
        }
        prev_read_val = read_val;
    }

    // print total received numbers and correctness
    printf("\nTotal received numbers: %i\n", received);
    printf("Correctness (ASC): %i\n", correct);

    // clean up
    free(read_buffer);
    sem_destroy(read_full);
    sem_destroy(read_empty);
    free(read_full);
    free(read_empty);

    // let master know that we are done
    sem_post(&sorted);
}

/**
 * Sends given value via buffer[write_in] and with waiting on write_empty and posting on write_full.
 *
 * @param write_buffer
 * @param write_empty
 * @param write_full
 * @param write_in
 * @param value
 * @return next write_in value
 */
int send_value(int *write_buffer, sem_t *write_empty, sem_t *write_full, int write_in, int value) {
    sem_wait(write_empty);
    //printf("Com: Put %i in buffer\n", value);
    write_buffer[write_in] = value;
    write_in = (write_in + 1) % buffer_size;
    sem_post(write_full);

    return write_in;
}

/**
 * Routine for a comparator thread.
 * Receives numbers and forwards the bigger one to the next comparator thread.
 *
 * @param p void pointer to a Comparator_params struct
 */
void *comparator(void *p) {
    // read buffer and semaphores
    Comparator_params *params = (Comparator_params*) p;
    sem_t *read_full = params->full;
    sem_t *read_empty = params->empty;
    int *read_buffer = params->buffer;
    int read_out = 0;

    // write buffer and semaphores
    int *write_buffer = malloc(sizeof(int) * buffer_size);
    int write_in = 0;
    sem_t *write_full = malloc(sizeof(sem_t));
    sem_t *write_empty = malloc(sizeof(sem_t));
    sem_init(write_full, 0, 0);
    sem_init(write_empty, 0, buffer_size);

    // currently read value
    int read_val;
    // the internal stored number of the comparator thread
    int stored_number;
    State state = INITIAL;

    while(1) {
        sem_wait(read_full);
        read_val = read_buffer[read_out];
        //printf("Com: Get %i from buffer\n", read_val);
        read_out = (read_out + 1) % buffer_size;
        sem_post(read_empty);

        /**
         * 4 different states for a comparator thread
         *
         * 1. INITIAL
         *      No number received yet. First number received -> stored_number. Next state: COMPARE_NO_THREAD
         *
         * 2. COMPARE_NO_THREAD
         *      There's no next thread yet. Depending on the received number there needs to be a comparator thread
         *      (next state: COMPARE) or if an END symbol is received an output thread created (next state: END).
         *      The received number/END symbol is then forwarded to the thread.
         *
         * 3. COMPARE
         *      All numbers received are compared to the stored_number and the lower one is sent to the next comparator
         *      thread. If an END symbol is received the end symbol and stored number are forwarded (next state: END).
         *
         * 4. END
         *      All numbers are forwarded to the next thread including the second END signal.
         *      Then the thread terminates.
         */
        if (state == INITIAL) {
            stored_number = read_val;
            //printf("Initial state: store number %i\n", read_val);
            state = COMPARE_NO_THREAD;
        } else if(state == COMPARE_NO_THREAD) {
            // prepare thread params
            Comparator_params params_next;
            params_next.empty = write_empty;
            params_next.full = write_full;
            params_next.buffer = write_buffer;

            if (read_val == -1) {
                // create output thread
                pthread_create(&thread, &attr, output, &params_next);

                // advance state to END
                state = END;

                // send end signal
                write_in = send_value(write_buffer, write_empty, write_full, write_in, read_val);
                // send stored_number
                write_in = send_value(write_buffer, write_empty, write_full, write_in, stored_number);
                continue;
            }

            // create comparator thread
            pthread_create(&thread, &attr, comparator, &params_next);

            // advance state to COMPARE
            state = COMPARE;

            // compare stored_number with read_val -> send smaller to next thread
            if (stored_number > read_val) {
                write_in = send_value(write_buffer, write_empty, write_full, write_in, read_val);
            } else {
                write_in = send_value(write_buffer, write_empty, write_full, write_in, stored_number);
                stored_number = read_val;
            }
        } else if(state == COMPARE) {
            if (read_val == -1) {
                // advance state to END
                state = END;

                // send end signal
                write_in = send_value(write_buffer, write_empty, write_full, write_in, read_val);
                // send stored_number
                write_in = send_value(write_buffer, write_empty, write_full, write_in, stored_number);
                continue;
            }

            // compare stored_number with read_val -> send smaller to next thread
            if (stored_number > read_val) {
                write_in = send_value(write_buffer, write_empty, write_full, write_in, read_val);
            } else {
                write_in = send_value(write_buffer, write_empty, write_full, write_in, stored_number);
                stored_number = read_val;
            }
        } else if (state == END) {
            // send every received number including second END, then terminate
            write_in = send_value(write_buffer, write_empty, write_full, write_in, read_val);

            if(read_val == -1) {
                //printf("Received second end signal %i\n", read_val);
                break; // terminate
            }
        }
    }

    // clean up
    free(read_buffer);
    sem_destroy(read_full);
    sem_destroy(read_empty);
    free(read_full);
    free(read_empty);
}

/**
 * Routine for a generator thread.
 * Generates length random numbers and passes them to a comparator thread.
 *
 * @param p void pointer to a Generator_params struct
 */
void *generator(void *p) {
    Generator_params *params = (Generator_params*) p;
    int length = params->length;

    // create buffer
    int *buffer = malloc(sizeof(int) * buffer_size);
    // create counter to write to buffer
    int next_in = 0;

    // create semaphores to protect buffer
    sem_t *full = malloc(sizeof(sem_t));
    sem_t *empty = malloc(sizeof(sem_t));
    sem_init(full, 0, 0);
    sem_init(empty, 0, buffer_size);

    // create first comparator thread
    Comparator_params params_next;
    params_next.empty = empty;
    params_next.full = full;
    params_next.buffer = buffer;
    pthread_create(&thread, &attr, comparator, &params_next);

    // send number by number into pipeline
    for(int i = 0; i < length; i++) {
        sem_wait(empty);
        //printf("Gen: Put %i in buffer\n", i);
        buffer[next_in] = rand();
        next_in = (next_in + 1) % buffer_size;
        sem_post(full);
    }

    // send 2 END symbols
    for(int i = 0; i < 2; i++) {
        sem_wait(empty);
        buffer[next_in] = -1;
        next_in = (next_in + 1) % buffer_size;
        sem_post(full);
    }
}