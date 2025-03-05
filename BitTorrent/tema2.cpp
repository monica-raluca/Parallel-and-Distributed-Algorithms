#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define REQUEST_FILE 2      // tag for requesting a file and receiving the wanted file information
#define FILE_DONE 11        // tag for letting the tracker know that a file was fully downloaded
#define FILES_COMPLETE 110  // tag for letting the tracker know that all files were fully downloaded
#define REQUEST_SEGMENT 3   // tag for requesting a segment (and depending on the source of the message, closing the upload thread)
#define ACK_SEGMENT 4       // tag for letting sending ack/nack for a segment
#define SEARCH_ALLOWED 111  // tag for letting the peer know that it can start searching for files

/*
 * Structure used in the initialize phaes
 * name - the name of the file
 * segments - the number of segments
 * hashes - the hashes of the file
 * users - the users that have the file (seed/peer)
 * ack - if searching for the desired files can start
*/
typedef struct fileInfo {
    char name[MAX_FILENAME];
    int segments;
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
    int users[10];
    bool ack;
} fileInfo;

/*
 * Structure for the searched file that is sent to a peer/seed
 * name - the name of the file
 * searchedSegment - the number of the segment the peer wants to download
*/
typedef struct searchedFile {
    char name[MAX_FILENAME];
    int searchedSegment;
} searchedFile;

/*
 * Structure for the file the peer wants to download
 * users - the users that have the file
 * hashes - the hashes of the file
 * totalSegments - the total number of segments
*/
typedef struct wantedFileInfo {
    int users[10];
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
    int totalSegments;
} wantedFileInfo;

/*
 * Structure for the peer's files
 * name - the name of the file
 * totalSegments - the total number of segments
 * downloadedSegments - the number of segments downloaded
 * hashes - the hashes of the file
 * wantedFile - if the file is already owned or wanted
*/
typedef struct userFile {
    char name[MAX_FILENAME];
    int totalSegments;
    int downloadedSegments;
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
    bool wantedFile;
} userFile;

static std::map<std::string, fileInfo> files;
static std::map<std::string, userFile> acquiredFiles;
static int wantedNumber;
static int clientsDone = 0;

MPI_Datatype MPI_fileData;
MPI_Datatype MPI_searchedFile;
MPI_Datatype MPI_wantedFileInfo;

void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;

    for (const auto &entry : acquiredFiles) {
        int downloadProgress = 0;
        // Start by asking the first peer from the seed/peer list and then go to the next one
        int currentPeer = 0;
        
        while (entry.second.downloadedSegments < entry.second.totalSegments) {
            wantedFileInfo wantedFileInfo;

            // If no segment was downloaded, get initial information from the tracker
            if (downloadProgress == 0) {
                MPI_Send(&entry.first[0], MAX_FILENAME, MPI_CHAR, TRACKER_RANK, REQUEST_FILE, MPI_COMM_WORLD);
                MPI_Recv(&wantedFileInfo, 1, MPI_wantedFileInfo, TRACKER_RANK, REQUEST_FILE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            }

            // Update map info with the data received from the tracker
            acquiredFiles[entry.first].totalSegments = wantedFileInfo.totalSegments;
            for (int j = 0; j < wantedFileInfo.totalSegments; j++) {
                strcpy(acquiredFiles[entry.first].hashes[j], wantedFileInfo.hashes[j]);
            }

            while (acquiredFiles[entry.first].downloadedSegments < wantedFileInfo.totalSegments) {
                if (entry.second.downloadedSegments == entry.second.totalSegments) {
                    break;
                }
                else {
                    searchedFile searched;
                    strcpy(searched.name, entry.first.c_str());
                    searched.searchedSegment = entry.second.downloadedSegments;

                    // Ask the peer in the currentPeer position
                    MPI_Send(&searched, 1, MPI_searchedFile, wantedFileInfo.users[currentPeer], REQUEST_SEGMENT, MPI_COMM_WORLD);
                    
                    bool ack;
                    MPI_Recv(&ack, 1, MPI_CXX_BOOL, wantedFileInfo.users[currentPeer], ACK_SEGMENT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    // The next peer will be asked for the next segment
                    currentPeer++;

                    // The peer doesn't ask itself for the segment
                    if (wantedFileInfo.users[currentPeer] == rank) {
                        currentPeer++;
                    }

                    // If the list of peers reached the end, start from the beginning
                    if (currentPeer == 10 || wantedFileInfo.users[currentPeer] == 0) {
                        currentPeer = 0;
                    }
                    
                    if (ack) {
                        acquiredFiles[entry.first].downloadedSegments++;

                        downloadProgress++;

                        // If the download progress reached 10, ask the tracker for the file information again
                        if (downloadProgress == 10) {
                            downloadProgress = 0;
                            MPI_Send(&entry.first[0], MAX_FILENAME, MPI_CHAR, TRACKER_RANK, REQUEST_FILE, MPI_COMM_WORLD);
                            MPI_Recv(&wantedFileInfo, 1, MPI_wantedFileInfo, TRACKER_RANK, REQUEST_FILE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                        }
                    }
                }
            }
        }

        // If the file was fully downloaded and the peer didn't have it initially, write it to a file and tell the tracker the file is downloaded
        if (entry.second.downloadedSegments == entry.second.totalSegments && acquiredFiles[entry.first].wantedFile) {
            MPI_Send(&entry.first[0], MAX_FILENAME, MPI_CHAR, TRACKER_RANK, FILE_DONE, MPI_COMM_WORLD);

            char fileName[MAX_FILENAME];
            sprintf(fileName, "client%d_%s", rank, entry.first.c_str());
            FILE *file = fopen(fileName, "w");
            if (file == NULL) {
                printf("Eroare la deschiderea fisierului\n");
                exit(-1);
            }

            for (int i = 0; i < entry.second.totalSegments; i++) {
                fprintf(file, "%s\n", entry.second.hashes[i]);
            }

            fclose(file);
        }
    }

    // If all files were downloaded, tell the tracker that the peer is done
    char filesComplete[MAX_FILENAME] = "filesComplete";
    MPI_Send(&filesComplete[0], MAX_FILENAME, MPI_CHAR, TRACKER_RANK, FILES_COMPLETE, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    searchedFile searched;
    MPI_Status status;

    while (1) {
        MPI_Recv(&searched, 1, MPI_searchedFile, MPI_ANY_SOURCE, REQUEST_SEGMENT, MPI_COMM_WORLD, &status);
        // If the message is from the tracker, this means all files were downloaded and the peer can stop the upload thread
        if (status.MPI_SOURCE == TRACKER_RANK) {
            break;
        }

        bool ack = false;

        // If the peer has the file and the segment was downloaded, make ack true
        if (acquiredFiles.find(searched.name) != acquiredFiles.end()) {
            if (acquiredFiles[searched.name].downloadedSegments >= searched.searchedSegment) {
                ack = true;
            }
        }
        MPI_Send(&ack, 1, MPI_CXX_BOOL, status.MPI_SOURCE, ACK_SEGMENT, MPI_COMM_WORLD);
    }

    return NULL;
}

void tracker(int numtasks, int rank) {
    for (int i = 1; i < numtasks; i++) {
        while (1) {
            fileInfo fileData;
            MPI_Recv(&fileData, 1, MPI_fileData, i, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            // If the file is already in the map, add the user to the list of users
            if (files.find(fileData.name) != files.end()) {
                fileInfo &refinedFileData = files[fileData.name];
                for (int j = 0; j < 1000; j++) {
                    if (refinedFileData.users[j] == 0) {
                        refinedFileData.users[j] = fileData.users[0];
                        break;
                    }
                }
            } else if (fileData.name[0] != '\0') {
                fileInfo refinedFileData;
                strcpy(refinedFileData.name, fileData.name);
                refinedFileData.segments = fileData.segments;
                for (int j = 0; j < fileData.segments; j++) {
                    strcpy(refinedFileData.hashes[j], fileData.hashes[j]);
                }

                memset(refinedFileData.users, 0, sizeof(refinedFileData.users));
                refinedFileData.users[0] = fileData.users[0];
                refinedFileData.ack = fileData.ack;

                files[fileData.name] = refinedFileData;
            }

            // If the peer sent its last file, go to the next peer
            if (fileData.ack) {
                break;
            }
        }
    }

    // Tell each peer they can start searching for files
    for (int i = 1; i < numtasks; i++) {
        bool searchAllowed = true;
        MPI_Send(&searchAllowed, 1, MPI_CXX_BOOL, i, SEARCH_ALLOWED, MPI_COMM_WORLD);
    }

    while(1) {
        char requestedFile[MAX_FILENAME];
        MPI_Status status;
        MPI_Recv(&requestedFile, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        
        if (status.MPI_TAG == REQUEST_FILE) {
            wantedFileInfo wantedFileInformation;
            fileInfo &sourceFile = files[requestedFile];

            wantedFileInformation.totalSegments = sourceFile.segments;

            for (int i = 0; i < sourceFile.segments; i++) {
                strcpy(wantedFileInformation.hashes[i], sourceFile.hashes[i]);
            }

            for (int i = 0; i < 10; i++) {
                wantedFileInformation.users[i] = sourceFile.users[i];
            }

            MPI_Send(&wantedFileInformation, 1, MPI_wantedFileInfo, status.MPI_SOURCE, REQUEST_FILE, MPI_COMM_WORLD);

            for (int i = 0; i < 10; i++) {
                if (sourceFile.users[i] == status.MPI_SOURCE) {
                    break;
                } else if (sourceFile.users[i] == 0) {
                    sourceFile.users[i] = status.MPI_SOURCE;
                    break;
                }
            }
        } else if (status.MPI_TAG == FILE_DONE) {
            fileInfo &sourceFile = files[requestedFile];

            for (int i = 0; i < 10; i++) {
                if (sourceFile.users[i] == status.MPI_SOURCE) {
                    break;
                } else if (sourceFile.users[i] == 0) {
                    sourceFile.users[i] = status.MPI_SOURCE;
                    break;
                }
            }
        } else if (status.MPI_TAG == FILES_COMPLETE) {
            // Another peer is now done
            clientsDone++;

            // If all the clients are done, send a message to all the clients to stop
            if (clientsDone == numtasks - 1) {
                searchedFile searched;
                for (int i = 1; i < numtasks; i++) {
                    MPI_Send(&searched, 1, MPI_searchedFile, i, REQUEST_SEGMENT, MPI_COMM_WORLD);
                }
                break;
            }
            
        }
    }
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    // Initilize data
    char fileName[MAX_FILENAME];
    sprintf(fileName, "in%d.txt", rank);
    int numberOfFiles;
    FILE *file = fopen(fileName, "r");
    if (file == NULL) {
        printf("Eroare la deschiderea fisierului\n");
        exit(-1);
    }

    fscanf(file, "%d", &numberOfFiles);

    // If the peer has no files, sends an empty fileInfo to the tracker
    if (numberOfFiles == 0) {
        fileInfo fileData;
        fileData.ack = true;
        fileData.name[0] = '\0';
        fileData.users[0] = rank;
        MPI_Send(&fileData, 1, MPI_fileData, TRACKER_RANK, 1, MPI_COMM_WORLD);
    }

    for (int i = 0; i < numberOfFiles; i++) {
        // Read the file data and build the structure to be sent to the tracker
        fileInfo fileData;

        fscanf(file, "%s", fileData.name);
        fscanf(file, "%d", &fileData.segments);

        for (int j = 0; j < fileData.segments; j++) {
            fscanf(file, "%s", fileData.hashes[j]);
        }

        fileData.users[0] = rank;

        // If the file is the last one, the ack is set to true (that peer finished sending the files)
        if (i == numberOfFiles - 1) {
            fileData.ack = true;
        } else {
            fileData.ack = false;
        }

        // Add the file to the acquiredFiles map, mark is as fully downloaded but not wanted
        acquiredFiles[fileData.name].totalSegments = fileData.segments;
        acquiredFiles[fileData.name].downloadedSegments = fileData.segments;
        for (int j = 0; j < fileData.segments; j++) {
            strcpy(acquiredFiles[fileData.name].hashes[j], fileData.hashes[j]);
        }
        acquiredFiles[fileData.name].wantedFile = false;

        MPI_Send(&fileData, 1, MPI_fileData, TRACKER_RANK, 1, MPI_COMM_WORLD);
    }
    
    bool ack = false;
    MPI_Recv(&ack, 1, MPI_CXX_BOOL, TRACKER_RANK, SEARCH_ALLOWED, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    fscanf(file, "%d", &wantedNumber);

    for (int i = 0; i < wantedNumber; i++) {
        userFile wantedFileInformation;
        fscanf(file, "%s", wantedFileInformation.name);
        wantedFileInformation.totalSegments = 1;
        wantedFileInformation.downloadedSegments = 0;
        wantedFileInformation.wantedFile = true;

        acquiredFiles[wantedFileInformation.name] = wantedFileInformation;
    }

    fclose(file);

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }

    MPI_Datatype oldtypes[5];
    int blockcounts[5];
    MPI_Aint offsets[5];

    offsets[0] = offsetof(fileInfo, name);
    oldtypes[0] = MPI_CHAR;
    blockcounts[0] = MAX_FILENAME;

    offsets[1] = offsetof(fileInfo, segments);
    oldtypes[1] = MPI_INT;
    blockcounts[1] = 1;

    offsets[2] = offsetof(fileInfo, hashes);
    oldtypes[2] = MPI_CHAR;
    blockcounts[2] = MAX_CHUNKS * (HASH_SIZE + 1);

    offsets[3] = offsetof(fileInfo, users);
    oldtypes[3] = MPI_INT;
    blockcounts[3] = 10;

    offsets[4] = offsetof(fileInfo, ack);
    oldtypes[4] = MPI_CXX_BOOL;
    blockcounts[4] = 1;

    MPI_Type_create_struct(5, blockcounts, offsets, oldtypes, &MPI_fileData);
    MPI_Type_commit(&MPI_fileData);

    MPI_Datatype oldtypes2[2];
    int blockcounts2[2];
    MPI_Aint offsets2[2];

    offsets2[0] = offsetof(searchedFile, name);
    oldtypes2[0] = MPI_CHAR;
    blockcounts2[0] = MAX_FILENAME;

    offsets2[1] = offsetof(searchedFile, searchedSegment);
    oldtypes2[1] = MPI_INT;
    blockcounts2[1] = 1;

    MPI_Type_create_struct(2, blockcounts2, offsets2, oldtypes2, &MPI_searchedFile);
    MPI_Type_commit(&MPI_searchedFile);

    MPI_Datatype oldtypes3[3];
    int blockcounts3[3];
    MPI_Aint offsets3[3];

    offsets3[0] = offsetof(wantedFileInfo, users);
    oldtypes3[0] = MPI_INT;
    blockcounts3[0] = 10;

    offsets3[1] = offsetof(wantedFileInfo, hashes);
    oldtypes3[1] = MPI_CHAR;
    blockcounts3[1] = MAX_CHUNKS * (HASH_SIZE + 1);

    offsets3[2] = offsetof(wantedFileInfo, totalSegments);
    oldtypes3[2] = MPI_INT;
    blockcounts3[2] = 1;

    MPI_Type_create_struct(3, blockcounts3, offsets3, oldtypes3, &MPI_wantedFileInfo);
    MPI_Type_commit(&MPI_wantedFileInfo);

    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}