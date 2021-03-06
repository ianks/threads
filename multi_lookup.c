#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/util.h"
#include "queue/queue.h"

#define MINARGS 3
#define USAGE "<inputFilePath> <outputFilePath>"
#define SBUFSIZE 1025
#define INPUTFS "%1024s"
#define NUM_THREADS 4

pthread_mutex_t queue_lock;
pthread_mutex_t dns_lock;
pthread_mutex_t incrementer_lock;
pthread_mutex_t file_lock;
pthread_mutex_t print_lock;
pthread_cond_t queue_not_full;
pthread_cond_t queue_not_empty;

int NUM_FILES;
int FILES_FINISHED_PROCESSING;
char* OUTFILE;
int queue_size;
queue address_queue;


/* Each thread reads it own txt file to avoid race conditions */
void* read_file(void* filename)
{
  char hostname[SBUFSIZE];

  /* Read File and Process*/
  FILE* file = fopen((char*) filename, "r");

  /* Scan through files, push hostname to queue while locked */
  while (fscanf(file, INPUTFS, hostname) > 0) {
    pthread_mutex_lock(&queue_lock);

    /* Check to see that there is space in the queue */
    while (queue_is_full(&address_queue)){
      printf("cond_wait q not full: %d\n", FILES_FINISHED_PROCESSING);
      fflush(stdout);
      pthread_cond_wait(&queue_not_full, &queue_lock);
      printf("cond_wait q not full exti: %d\n", FILES_FINISHED_PROCESSING);
    }

    queue_push(&address_queue, hostname);
    pthread_cond_signal(&queue_not_empty);

    pthread_mutex_unlock(&queue_lock);
  }

  pthread_mutex_lock(&incrementer_lock);
  FILES_FINISHED_PROCESSING++;
  pthread_mutex_unlock(&incrementer_lock);

  fclose(file);

  return NULL;
}


void* dns_output()
{
  char firstipstr[INET6_ADDRSTRLEN];
  FILE* outputfp = fopen(OUTFILE, "w");

  while(1){
    pthread_mutex_lock(&queue_lock);

    while(queue_is_empty(&address_queue)){

      /* Queue is empty and no more files left, we're done */
      pthread_mutex_lock(&incrementer_lock);
      int EXIT_THREAD = (FILES_FINISHED_PROCESSING == NUM_FILES);
      pthread_mutex_unlock(&incrementer_lock);

      if (EXIT_THREAD){
        printf("All files have been processed. Return.\n");

        /* Make sure we drop the lock before exiting routine */
        pthread_mutex_unlock(&queue_lock);
        return NULL;
      }

      /* Wait until the producers add something to queue */
      pthread_cond_wait(&queue_not_empty, &queue_lock);
    }

    printf("Continue lookup. File number: %d / %d\n", FILES_FINISHED_PROCESSING, NUM_FILES);
    fflush(stdout);

    char* hostname = (char*) queue_pop(&address_queue);
    pthread_cond_signal(&queue_not_full);

    /* We have to copy hostname here to avoid segfault */
    /* Tell me why and I'll give you a dollar */
    char* hostname_copy = malloc(sizeof(char) * strlen(hostname));
    strcpy(hostname_copy, hostname);

    if(dnslookup(hostname_copy, firstipstr, sizeof(firstipstr)) == UTIL_FAILURE) {
      fprintf(stderr, "dnslookup error: %s\n", hostname);
      strncpy(firstipstr, "", sizeof(firstipstr));
    }

    /* Print to file */
    pthread_mutex_lock(&print_lock);
    fprintf(outputfp, "%s, %s\n", hostname_copy, firstipstr);
    pthread_mutex_unlock(&print_lock);

    /* Don't memory leak */
    free(hostname_copy);

    /* Unlock queue */
    pthread_mutex_unlock(&queue_lock);
  }

  fclose(outputfp);

  return NULL;
}


void* res_pool()
{
  pthread_t res_threads[NUM_FILES];

  /* Thread pool for reading files */
  int i;
  for (i = 0; i < NUM_FILES; i++) {
    pthread_create(&(res_threads[i]), NULL, dns_output, NULL);
    pthread_join(res_threads[i], NULL);
  }

  return NULL;
}

void* req_pool(void* files)
{
  char** filenames = (char**) files;
  pthread_t req_threads[NUM_FILES];

  /* Thread pool for reading files */
  int i;
  for (i = 0; i < NUM_FILES; i++) {
    char* filename = filenames[i];
    pthread_create(&(req_threads[i]), NULL, read_file, (void*) filename);
    pthread_join(req_threads[i], NULL);
  }

  for (i = 0; i < NUM_FILES; i++)
    pthread_cond_signal(&queue_not_empty);

  return NULL;
}


void init_variables(int argc)
{
  FILES_FINISHED_PROCESSING = 0;
  NUM_FILES = argc - 2;
  queue_init(&address_queue, 16);
  pthread_cond_init(&queue_not_full, NULL);
  pthread_cond_init(&queue_not_empty, NULL);
  pthread_mutex_init(&queue_lock, NULL);
  pthread_mutex_init(&print_lock, NULL);
  pthread_mutex_init(&file_lock, NULL);
  pthread_mutex_init(&incrementer_lock, NULL);
}


int main(int argc, char* argv[])
{
  /* Check arguments */
  if(argc < MINARGS) {
    fprintf(stderr, "Not enough arguments: %d\n", (argc - 1));
    fprintf(stderr, "Usage:\n %s %s\n", argv[0], USAGE);

    return EXIT_FAILURE;
  }

  /* Where we write our output to */
  OUTFILE = argv[argc - 1];

  /* Initialize vars */
  init_variables(argc);

  /* Extract the filenames from argv */
  char* filenames[NUM_FILES];
  int i;
  for (i = 1; i < (argc - 1); i++)
    filenames[i - 1] = argv[i];

  /* IDs for producer and consumer threads */
  pthread_t producer, consumer;

  /* Start producer thread pool */
  int producer_thread;
  producer_thread = pthread_create(&(producer), NULL, req_pool, (void*)filenames);

  /* Start consumer thread pool */
  int consumer_thread;
  consumer_thread = pthread_create(&(consumer), NULL, res_pool, NULL);

  /* Wait for consumer to finish before main exits */
  pthread_join(consumer, NULL);

  /* Be cleanly */
  pthread_mutex_destroy(&queue_lock);
  pthread_mutex_destroy(&file_lock);
  queue_cleanup(&address_queue);

  return EXIT_SUCCESS;
}
