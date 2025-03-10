#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <queue>
#include <cmath>

#define min(a, b) (((a) < (b)) ? (a) : (b))

/**
 * Structure that holds the data that is passed to each thread
 * threadID - the ID of the thread
 * numberOfMappers - the number of mapper threads
 * currentLetter - the current letter that is being processed
 * currentGroup - the current group that is being processed
 * filesQueue - a queue that holds the files that need to be processed
 * wordMaps - a queue that holds the maps that are generated by the mappers
 * reducerOutput - a map that holds the output generated by the reducers
 * groupedWords - a map that holds the words grouped by the starting letter, along with the file indexes
 * barrier - a barrier that is used to synchronize all the threads
 * reducerBarrier - a barrier that is used to synchronize only the reducers
 * mutex - a mutex that is used to synchronize the access to shared data
*/
typedef struct {
    int threadID;
    int numberOfMappers;
    char *currentLetter;
    char *currentGroup;
    std::queue<std::pair<char *, int>> *filesQueue;
    std::queue<std::unordered_map<std::string, int>> *wordMaps;
    std::unordered_map<std::string, std::vector<int>> *reducerOutput;
    std::unordered_map<char, std::vector<std::pair<std::string, std::vector<int>>>> *groupedWords;
    pthread_barrier_t *barrier;
    pthread_barrier_t *reducerBarrier;
    pthread_mutex_t *mutex;
} ThreadData;

/**
 * Function that modifies a word, only leaving the letters
 * and converting them to lowercase
*/
void modifyWord(char *word) {
    char *newWord = (char *)malloc(100 * sizeof(char));
    if (newWord == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(0);
    }

    int letters = 0;
    
    // Copy only alpha characters and convert them to lowercase
    for (size_t i = 0; i < strlen(word); i++) {
        if (isalpha(word[i])) {
            newWord[letters++] = tolower(word[i]);
        }
    }

    // Add the null string terminator and copy the new word back
    newWord[letters] = '\0';
    strcpy(word, newWord);

    free(newWord);
}

/**
 * Function that processes a file and returns a map
 * that associates each word with the file index it was found in
*/
std::unordered_map<std::string, int> processFile(const char *filename, int fileID) {
    std::unordered_map<std::string, int> wordMap;

    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        fprintf(stderr, "File not found: %s\n", filename);
        exit(0);
    }

    char *word = (char *)malloc(100 * sizeof(char));
    if (word == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(0);
    }

    // Read each word from the file and add it to the map after modifying it
    while (fscanf(file, "%s", word) != EOF) {
        modifyWord(word);
        std::string stringWord(word);

        if (!stringWord.empty()) {
            wordMap[stringWord] = fileID;
        }
    }

    fclose(file);
    free(word);

    return wordMap;
}

void *threadFunction(void *arg) {
    ThreadData *threadData = (ThreadData *)arg;
    int threadID = threadData->threadID;
    int numberOfMappers = threadData->numberOfMappers;
    char *currentLetter = threadData->currentLetter;
    char *currentGroup = threadData->currentGroup;
    std::queue<std::pair<char *, int>> *filesQueue = threadData->filesQueue;
    std::queue<std::unordered_map<std::string, int>> *wordMaps = threadData->wordMaps;
    std::unordered_map<std::string, std::vector<int>> *reducerOutput = threadData->reducerOutput;
    std::unordered_map<char, std::vector<std::pair<std::string, std::vector<int>>>> *groupedWords = threadData->groupedWords;
    pthread_barrier_t *barrier = threadData->barrier;
    pthread_barrier_t *reducerBarrier = threadData->reducerBarrier;
    pthread_mutex_t *mutex = threadData->mutex;

    // Tasks executed by the mapper threads
    if (threadID < numberOfMappers) {
        // Iterate through the files and process them until the queue is empty
        while (!filesQueue->empty()) {
            pthread_mutex_lock(mutex);

            if (filesQueue->empty()) {
                pthread_mutex_unlock(mutex);
                break;
            }

            // Take the next file to process
            char *filename = filesQueue->front().first;
            int fileID = filesQueue->front().second;
            filesQueue->pop();
            pthread_mutex_unlock(mutex);

            // Process the file and get the resulted map
            std::unordered_map<std::string, int> wordMap = processFile(filename, fileID);

            // Add the map to the queue that the reducers will process
            pthread_mutex_lock(mutex);
            wordMaps->push(wordMap);
            pthread_mutex_unlock(mutex);

            free(filename);
        }


        pthread_barrier_wait(barrier);
    } else {
        // Wait for the mappers to finish processing the files
        pthread_barrier_wait(barrier);

        std::unordered_map<std::string, std::vector<int>> partialMap;
        std::unordered_map<std::string, int> wordMap;

        while (true) {
            pthread_mutex_lock(mutex);
            if (wordMaps->empty()) {
                pthread_mutex_unlock(mutex);
                break;
            }
            
            // Remove one map from the queue
            wordMap = wordMaps->front();
            wordMaps->pop();
            pthread_mutex_unlock(mutex);

            // Add the words to the partial map
            for (const auto &entry : wordMap) {
                const std::string &word = entry.first;
                int fileIndex = entry.second;

                if (partialMap.find(word) == partialMap.end()) {
                    partialMap[word] = {fileIndex};
                } else {
                    partialMap[word].push_back(fileIndex);
                }
            }
        }
        
        // Copy the partial map to the reducer output
        pthread_mutex_lock(mutex);
        for (const auto &entry : partialMap) {
            const std::string &word = entry.first;
            const std::vector<int> &fileIndexes = entry.second;

            if (reducerOutput->find(word) == reducerOutput->end()) {
                (*reducerOutput)[word] = fileIndexes;
            } else {
                (*reducerOutput)[word].insert((*reducerOutput)[word].end(), fileIndexes.begin(), fileIndexes.end());
            }
        }
        pthread_mutex_unlock(mutex);

        pthread_barrier_wait(reducerBarrier);
        
        // Group the words by their starting letter
        if (threadID - numberOfMappers == 0) {
            for (const auto &entry : *reducerOutput) {
                char startingLetter = entry.first[0];
                (*groupedWords)[startingLetter].emplace_back(entry.first, entry.second);
            }
        }

        pthread_barrier_wait(reducerBarrier);

        /* Sort the words in each group
           Each reducer will sort the words starting with a specific letter */
        while (true) {
            pthread_mutex_lock(mutex);

            // Stop when all the letters have been processed
            if (*currentLetter > 'z') {
                pthread_mutex_unlock(mutex);
                break;
            }

            // Take the current letter and then increment it
            char letter = *currentLetter;
            (*currentLetter)++;

            pthread_mutex_unlock(mutex);

            auto &words = (*groupedWords)[letter];
            
            // Sort the indices 
            for (auto &word : words) {
                std::sort(word.second.begin(), word.second.end());
            }

            /* Sort the words by the number of files they appear in
               If the number of files is the same, sort them alphabetically */
            std::sort(words.begin(), words.end(), [](const auto &a, const auto &b) {
                if (a.second.size() != b.second.size()) {
                    return a.second.size() > b.second.size();
                }
                return a.first < b.first;
            });
        }

        pthread_barrier_wait(reducerBarrier);
        
        // Write the output to the files
        while (true) {
            pthread_mutex_lock(mutex);

            // Stop when all the groups have been processed
            if (*currentGroup > 'z') {
                pthread_mutex_unlock(mutex);
                break;
            }

            // Take the current group and then increment it
            char group = *currentGroup;
            (*currentGroup)++;

            pthread_mutex_unlock(mutex);

            // Open the file starting with the letter of the group being processed
            auto &words = (*groupedWords)[group];
            char filename[6];
            snprintf(filename, sizeof(filename), "%c.txt", group);

            FILE *file = fopen(filename, "w");
            if (file == NULL) {
                fprintf(stderr, "Failed to open file: %s\n", filename);
                exit(0);
            }

            // Write the words to the file in the specified format
            for (size_t i = 0; i < words.size(); i++) {
                const auto &entry = words[i];
                fprintf(file, "%s:[", entry.first.c_str());

                for (size_t j = 0; j < entry.second.size(); j++) {
                    fprintf(file, "%d", entry.second[j]);
                    if (j < entry.second.size() - 1) {
                        fprintf(file, " ");
                    }
                }
                fprintf(file, "]\n");
            }

            fclose(file);
        }
    }

    pthread_exit(NULL);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: ./tema1 <numberOfMappers> <numberOfReducers> <inputFile>\n");
        exit(0);
    }

    int numberOfMappers = atoi(argv[1]);
    int numberOfReducers = atoi(argv[2]);
    int totalThreads = numberOfMappers + numberOfReducers;
    char *inputFile = argv[3];

    FILE *file = fopen(inputFile, "r");
    if (file == NULL) {
        fprintf(stderr, "File not found\n");
        exit(0);
    }

    int numberOfFiles = 0;
    fscanf(file, "%d", &numberOfFiles);

    std::queue<std::pair<char *, int>> filesQueue;

    // Add the files to be processed to the queue
    for (int i = 0; i < numberOfFiles; i++) {
        char *fileName = (char *)malloc(100 * sizeof(char));
        if (fileName == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(0);
        }
        fscanf(file, "%s", fileName);
        filesQueue.push(std::make_pair(fileName, i + 1));
    }
    fclose(file);

    char currentLetter = 'a';
    char currentGroup = 'a';
    std::queue<std::unordered_map<std::string, int>> wordMaps;
    std::unordered_map<std::string, std::vector<int>> reducerOutput;
    std::queue<std::unordered_map<std::string, std::vector<int>>> reducerQueue;
    std::unordered_map<char, std::vector<std::pair<std::string, std::vector<int>>>> groupedWords;

    pthread_t *threads = (pthread_t *)malloc(totalThreads * sizeof(pthread_t));
    if (threads == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(0);
    }

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, totalThreads);

    pthread_barrier_t reducerBarrier;
    pthread_barrier_init(&reducerBarrier, NULL, numberOfReducers);

    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, NULL);

    for (int i = 0; i < totalThreads; i++) {
        ThreadData *threadData = (ThreadData *)malloc(sizeof(ThreadData));
        if (threadData == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(0);
        }

        threadData->threadID = i;
        threadData->numberOfMappers = numberOfMappers;
        threadData->currentLetter = &currentLetter;
        threadData->currentGroup = &currentGroup;
        threadData->filesQueue = &filesQueue;
        threadData->wordMaps = &wordMaps;
        threadData->reducerOutput = &reducerOutput;
        threadData->groupedWords = &groupedWords;
        threadData->barrier = &barrier;
        threadData->reducerBarrier = &reducerBarrier;
        threadData->mutex = &mutex;

        int rc = pthread_create(&threads[i], NULL, threadFunction, threadData);
        if (rc) {
            fprintf(stderr, "Error creating thread\n");
            exit(0);
        }
    }

    for (int i = 0; i < totalThreads; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc) {
            fprintf(stderr, "Error joining thread\n");
            exit(0);
        }
    }

    pthread_barrier_destroy(&barrier);
    pthread_barrier_destroy(&reducerBarrier);
    pthread_mutex_destroy(&mutex);

    free(threads);

    return 0;
}