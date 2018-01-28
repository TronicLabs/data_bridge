/// plugin.cpp
///
/// (c) 2018 Pongasoft and bsp
///
///   Licensed under the Apache License, Version 2.0 (the "License");
///   you may not use this file except in compliance with the License.
///   You may obtain a copy of the License at
///
///       http://www.apache.org/licenses/LICENSE-2.0
///
///   Unless required by applicable law or agreed to in writing, software
///   distributed under the License is distributed on an "AS IS" BASIS,
///   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
///   See the License for the specific language governing permissions and
///   limitations under the License.
///
/// changed: 27Jan2018
///
///
/// (note) the Linux/MacOSX related POSIX codepaths are completely untested (they may not even compile)
///
///

#include <aeffect.h>
#include <aeffectx.h>
#include <stdio.h>

#if defined(_WIN32) || defined(_WIN64)
#define HAVE_WINDOWS defined

#define WIN32_LEAN_AND_MEAN defined
#include <windows.h>

// Windows:
#define VST_EXPORT  extern "C" __declspec(dllexport)

struct PluginMutex {
   CRITICAL_SECTION handle; 

   PluginMutex(void) {
      ::InitializeCriticalSection( &handle ); 
   }

   ~PluginMutex() {
      ::DeleteCriticalSection( &handle ); 
   }

   void lock(void) {
      ::EnterCriticalSection(&handle); 
   }

   void unlock(void) {
      ::LeaveCriticalSection(&handle); 
   }
};
#else

// MacOSX, Linux:
#define HAVE_UNIX defined

#define VST_EXPORT extern

#include <pthread.h> 
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

//static pthread_mutex_t loc_pthread_mutex_t_init = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t loc_pthread_mutex_t_init = PTHREAD_MUTEX_INITIALIZER;

struct PluginMutex {
   pthread_mutex_t handle;

   PluginMutex(void) {
      ::memcpy((void*)&handle, (const void*)&loc_pthread_mutex_t_init, sizeof(pthread_mutex_t));
   }

   ~PluginMutex() {
   }

   void lock(void) {
		::pthread_mutex_lock(&handle); 
   }

   void unlock(void) {
		::pthread_mutex_unlock(&handle); 
   }
};

#endif // _WIN32||_WIN64


extern "C" {
// <class.png>
// data_bridge memory mapped i/o communication structure 
typedef struct databridge_channel_s {
   static const uint32_t CHUNK_NUM_FRAMES =  64u;  // number of sample frames per buffer chunk
   static const uint32_t NUM_CHUNKS       = 256u;  // maximum number of ringbuffer chunks
                                                   // (16 or less used for ASIO, 256 used only for MME output@4096 frames)
   // (note) communication mutices are called "vst_databridge_io_<chidx>"
   uint32_t next_read_chunk_idx;  // <consumer> wraps around
   uint32_t num_chunks_avail;     // <producer> current number of available ringbuffer chunks
   float sample_data[CHUNK_NUM_FRAMES * NUM_CHUNKS * 2/*interleaved stereo*/];

} databridge_channel_t;

typedef struct databridge_s {
   static const int NUM_CHANNELS = 16;  // number of i/o channels (n inputs, n outputs)
   uint32_t             version;     // 1, written by first connecting process
   uint8_t              resvd[252];  // reserved for future extensions
   databridge_channel_t channels[NUM_CHANNELS];
} databridge_t;

#define DATABRIDGE_VERSION              1u
#define DATABRIDGE_SHM_NAME             "vst_databridge"      // (system-wide) shared memory name
#define DATABRIDGE_SHM_SIZE             sizeof(databridge_t)  // size of shared memory region
#define DATABRIDGE_MTX_PREFIX           "vst_databridge_io_"  // prefix for channel mutices
#define DATABRIDGE_TIMEOUT_MILLISEC     (500)                 // channel mutex timeout
#define DATABRIDGE_HOST_EVENT_NAME      "vst_databridge_ev"   // raised by any consumer, databridge-only host can wait for it
#define effDataBridgeGetNumChunksAvail  0x44894489            // effect extension opcode, used by databridge-only host
};



/*
 * I find the naming a bit confusing so I decided to use more meaningful names instead.
 */

/**
 * The VSTHostCallback is a function pointer so that the plugin can communicate with the host (not used in this small example)
 */
typedef audioMasterCallback VSTHostCallback;

/**
 * The VSTPlugin structure (AEffect) contains information about the plugin (like version, number of inputs, ...) and
 * callbacks so that the host can call the plugin to do its work. The primary callback will be `processReplacing` for
 * single precision (float) sample processing (or `processDoubleReplacing` for double precision (double)).
 */
typedef AEffect VSTPlugin;


// Since the host is expecting a very specific API we need to make sure it has C linkage (not C++)
extern "C" {

/*
 * This is the main entry point to the VST plugin.
 *
 * The host (DAW like Maschine, Ableton Live, Reason, ...) will look for this function with this exact API.
 *
 * It is the equivalent to `int main(int argc, char *argv[])` for a C executable.
 *
 * @param vstHostCallback is a callback so that the plugin can communicate with the host (not used in this small example)
 * @return a pointer to the AEffect structure
 */
VST_EXPORT VSTPlugin *VSTPluginMain (VSTHostCallback vstHostCallback);

// note this looks like this without the type aliases (and is obviously 100% equivalent)
// extern AEffect *VSTPluginMain(audioMasterCallback audioMaster);

}

/*
 * Constant for the version of the plugin. For example 1100 for version 1.1.0.0
 */
const VstInt32 PLUGIN_VERSION = 1000;


/**
 * Encapsulates the plugin as a C++ class. It will keep both the host callback and the structure required by the
 * host (VSTPlugin). This class will be stored in the `VSTPlugin.object` field (circular reference) so that it can
 * be accessed when the host calls the plugin back (for example in `processDoubleReplacing`).
 */
class VSTPluginWrapper {
   static const uint32_t MIN_SAMPLE_RATE = 8192u;  // (note) cannot be float in C++
   static const uint32_t MAX_SAMPLE_RATE = 384000u;
   static const uint32_t MIN_BLOCK_SIZE  = 64u;
   static const uint32_t MAX_BLOCK_SIZE  = 4096u;

protected:
   float    sample_rate;   // e.g. 44100.0
   uint32_t block_size;    // e.g. 64

   PluginMutex mtx_audio;

   bool                   b_shm_owner;
   volatile databridge_t *shm;

public:
#ifdef HAVE_WINDOWS
   HANDLE        shm_handle;
   HANDLE        mtx_databridge_io[databridge_t::NUM_CHANNELS];
   HANDLE        host_ev;
#elif defined(HAVE_UNIX)
   int           shm_handle;
   int           mtx_databridge_io[databridge_t::NUM_CHANNELS];
   // (todo) host_ev
#endif

   bool b_processing;  // true=generate output, false=suspended

   struct {
      bool     b_input;
      uint32_t channel_idx;
   } params;

   uint32_t num_underruns;

   uint32_t        chunk_frames_left;
   union {
      const volatile float *in;   // shm input
      volatile       float *out;  // shm output
      void                 *any;
   } sample_ptr;
   bool            b_unlock_io_mutex;

   bool b_force_reset_chunk_idx;  // true=when channel reader just connected (=> sync with producer)

   uint32_t debug_output_frame_count;


public:
   VSTPluginWrapper(VSTHostCallback vstHostCallback,
                    VstInt32 vendorUniqueID,
                    VstInt32 vendorVersion,
                    VstInt32 numParams,
                    VstInt32 numPrograms,
                    VstInt32 numInputs,
                    VstInt32 numOutputs
                    );

   ~VSTPluginWrapper();

   VSTPlugin *getVSTPlugin(void) {
      return &_vstPlugin;
   }

   void lockAudio(void) {
      mtx_audio.lock();
   }

   void unlockAudio(void) {
      mtx_audio.unlock();
   }

   VstInt32 getNumInputs(void) const {
      return _vstPlugin.numInputs;
   }

   VstInt32 getNumOutputs(void) const {
      return _vstPlugin.numOutputs;
   }

   bool setSampleRate(float _rate) {
      bool r = false;

      if((_rate >= float(MIN_SAMPLE_RATE)) && (_rate <= float(MAX_SAMPLE_RATE)))
      {
         lockAudio();
         sample_rate = _rate;
         unlockAudio();
         r = true;
      }

      return r;
   }

   bool setBlockSize(uint32_t _blockSize) {
      bool r = false;

      if((_blockSize >= MIN_BLOCK_SIZE) && (_blockSize <= MAX_BLOCK_SIZE))
      {
         lockAudio();
         block_size = _blockSize;
         unlockAudio();
         r = true;
      }

      return r;
   }

   void setEnableProcessingActive(bool _bEnable) {
      lockAudio();
      b_processing = _bEnable;

      if(b_processing && params.b_input)
      {
         b_force_reset_chunk_idx = true;
      }

      unlockAudio();
   }

   bool setBankChunk(size_t _size, uint8_t*_addr) {
      bool r = false;
      lockAudio();
      // ...
      unlockAudio();
      return r;
   }

   bool setProgramChunk(size_t _size, uint8_t*_addr) {
      bool r = false;
      lockAudio();
      // ...
      unlockAudio();
      return r;
   }

   bool connect(void) {
      // called via effOpen()
      lockAudio();
      bool r = false;

      bool bCreate = false;

      if(NULL == shm)
      {
         b_shm_owner = false;

#ifdef HAVE_WINDOWS
         HANDLE hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS,   // read/write access
                                           FALSE,                 // do not inherit the name
                                           (char*)DATABRIDGE_SHM_NAME  // name of mapping object 
                                           );
      
         if(NULL != hMapFile) 
         { 
            // Already exists
            printf("data_bridge:connect: ok, opened file mapping (name=\"%s\" size=%zu)\n", 
                   DATABRIDGE_SHM_NAME,
                   DATABRIDGE_SHM_SIZE
                   );
      
            shm = (volatile databridge_t*) ::MapViewOfFile(hMapFile,    // handle to mapping object
                                                           FILE_MAP_ALL_ACCESS,  // read/write permission
                                                           0,                    // <dwFileOffsetHigh>
                                                           0,                    // <dwFileOffsetLow>
                                                           DATABRIDGE_SHM_SIZE
                                                           );
      
            if(NULL == shm)
            { 
               // (note) this happened after a few dozen test runs
               printf("data_bridge:connect: failed to map view of file (name=\"%s\" size=%zu).\n", 
                      DATABRIDGE_SHM_NAME,
                      DATABRIDGE_SHM_SIZE
                      );
               ::CloseHandle(hMapFile);
               bCreate = true;
            }
            else
            {
               printf("data_bridge:connect: ok, mapped view of file (name=\"%s\" size=%zu).\n", 
                      DATABRIDGE_SHM_NAME,
                      DATABRIDGE_SHM_SIZE
                      );
            }
         }
         else
         {
            bCreate = true;
         }

         if(bCreate)
         {
            // Create
            hMapFile = ::CreateFileMapping(INVALID_HANDLE_VALUE,    // use paging file
                                           NULL,                    // default security 
                                           PAGE_READWRITE,          // read/write access
                                           0,                       // max. object size 
                                           DATABRIDGE_SHM_SIZE,     // buffer size  
                                           DATABRIDGE_SHM_NAME      // name of mapping object
                                           );
            
            if( (NULL == hMapFile) || (INVALID_HANDLE_VALUE == hMapFile) ) 
            { 
               printf("data_bridge:connect: failed to create file mapping object (name=\"%s\" size=%zu). GetLastError=%d\n", 
                      DATABRIDGE_SHM_NAME,
                      DATABRIDGE_SHM_SIZE,
                      GetLastError()
                      );
            }
            else
            {
               printf("data_bridge:connect: ok, created file mapping object (name=\"%s\" size=%zu).\n", 
                      DATABRIDGE_SHM_NAME,
                      DATABRIDGE_SHM_SIZE
                      );

               shm = (volatile databridge_t*) MapViewOfFile(hMapFile,            // handle to map object
                                                            FILE_MAP_ALL_ACCESS, // read/write permission
                                                            0,                   // <dwFileOffsetHigh>
                                                            0,                   // <dwFileOffsetLow>
                                                            DATABRIDGE_SHM_SIZE
                                                            );
            
               if(NULL == shm)
               { 
                  printf("data_bridge:connect: failed to map view of file (name=\"%s\" size=%zu).\n", 
                         DATABRIDGE_SHM_NAME,
                         DATABRIDGE_SHM_SIZE
                         );
                  ::CloseHandle(hMapFile);
                  return false;
               }

               printf("data_bridge:connect: ok, mapped view of file (name=\"%s\" size=%zu).\n", 
                      DATABRIDGE_SHM_NAME,
                      DATABRIDGE_SHM_SIZE
                      );

               shm_handle = hMapFile;

               // Init databridge
               ::memset((void*)shm, 0, DATABRIDGE_SHM_SIZE);
               shm->version = DATABRIDGE_VERSION;
               b_shm_owner = true;
            }

         } // connect or create

         if(NULL != shm)
         {
            // Open or create global channel locks
            char mtxName[256];

            for(uint32_t channelIdx = 0u; channelIdx < databridge_t::NUM_CHANNELS; channelIdx++)
            {
               // Create or open channel mutex
               ::snprintf(mtxName, 256, DATABRIDGE_MTX_PREFIX "%u", channelIdx);

               mtx_databridge_io[channelIdx] = ::CreateMutex(NULL/*lpMutexAttributes*/,
                                                             FALSE/*bInitialOwner*/,
                                                             mtxName
                                                             );
            }

            // Open or create host event (optional, for databridge-only hosts)
            host_ev = ::CreateEvent(NULL/*lpEventAttributes*/,
                                    FALSE/*bManualReset*/,
                                    FALSE/*bInitialState*/,
                                    DATABRIDGE_HOST_EVENT_NAME
                                    );


            // Succeeded
            r = true;
         } // if shm

#elif defined(HAVE_UNIX)
         int fdMapFile = ::open("/tmp/" DATABRIDGE_SHM_NAME, O_RDWR);

         if(-1 == fdMapFile)
         { 
            // Does not exist, create it
            fdMapFile = ::open("/tmp/" DATABRIDGE_SHM_NAME, O_RDWR | O_CREAT, S_IRWXU);

            if(-1 == fdMapFile)
            {
               printf("data_bridge:connect: failed to create mmap file \"%s\". errno=%i (%m).\n", 
                      "/tmp/" DATABRIDGE_SHM_NAME,
                      errno
                      );
            }
            else
            {
               // Truncate the file to the desired shared memory size.
               ::ftruncate(fdMapFile, DATABRIDGE_SHM_SIZE);

               printf("data_bridge:connect: ok, created mmap file \"%s\"\n", "/tmp/" DATABRIDGE_SHM_NAME);

               b_shm_owner = true;
            }
         }
         else
         {
            printf("data_bridge:connect: ok, opened mmap file \"%s\"\n", "/tmp/" DATABRIDGE_SHM_NAME);
         }

         if(-1 != fdMapFile)
         {
            // Now memory map the file 
            shm = (volatile databridge_t*) ::mmap(NULL,
                                                  DATABRIDGE_SHM_SIZE,
                                                  PROT_READ|PROT_WRITE,
                                                  MAP_SHARED, 
                                                  fdMapFile,
                                                  0
                                                  );

            if((sU8*)-1 == shm)
            { 
               printf("data_bridge:connect: failed to mmap shared memory (size=%u). errno=%d.\n", 
                      DATABRIDGE_SHM_SIZE,
                      errno
                      ); 
               shm = NULL;
               ::close(fdMapFile);
               shm_handle = -1;
               return false;
            }

            shm_handle = fdMapFile;

            if(b_shm_owner)
            {
               // Init databridge
               ::memset((void*)shm, 0, DATABRIDGE_SHM_SIZE);
               shm->version = DATABRIDGE_VERSION;

               // Create global channel locks
               char mtxName[256];

               for(uint32_t channelIdx = 0u; channelIdx < databridge_t::NUM_CHANNELS; channelIdx++)
               {
                  // Create channel mutex
                  ::snprintf(mtxName, 256, "/tmp/" DATABRIDGE_MTX_PREFIX "%u", channelIdx);
                  mtx_databridge_io[channelIdx] = ::open(mtxName, O_RDWR | O_CREAT, S_IRWXU);
               }

               // (todo) Create host event (optional, for databridge-only hosts)
            }
            else
            {
               // Open global channel mutices
               char mtxName[256];

               for(uint32_t channelIdx = 0u; channelIdx < databridge_t::NUM_CHANNELS; channelIdx++)
               {
                  // Open input channel mutex
                  ::snprintf(mtxName, 256, "/tmp/" DATABRIDGE_MTX_PREFIX "%u", channelIdx);
                  mtx_databridge_io[channelIdx] = ::open(mtxName, O_RDWR, S_IRWXU);
               }

               // (todo) Open host event (optional, for databridge-only hosts)
            }

            // Succeeded
            r = true;

         } // if -1 != fdMapFile
#endif
      } // if NULL==shm

      unlockAudio();

      return r;
   }

   void disconnect(void) {
      // called via effClose() (or via d'tor)
      lockAudio();

      if(b_unlock_io_mutex)
      {
         unlockChannelMutex(mtx_databridge_io[params.channel_idx]);
         b_unlock_io_mutex = false;
      }

      if(NULL != shm)
      {
#ifdef HAVE_WINDOWS
         // Close global channel locks
         for(uint32_t channelIdx = 0u; channelIdx < databridge_t::NUM_CHANNELS; channelIdx++)
         {
            ::CloseHandle(mtx_databridge_io[channelIdx]);
         }

         // Unmap shared memory
         ::UnmapViewOfFile((LPCVOID)shm);
         shm = NULL;
         ::CloseHandle(shm_handle);
         shm_handle = NULL;

         // Close host event handle
         ::CloseHandle(host_ev);
         host_ev = NULL;

#elif defined(HAVE_UNIX)
         // Close global channel locks
         for(uint32_t channelIdx = 0u; channelIdx < databridge_t::NUM_CHANNELS; channelIdx++)
         {
            ::close(mtx_databridge_io[channelIdx]);
         }

         // Unmap shared memory
         ::munmap((void*) shm, DATABRIDGE_SHM_SIZE);
         shm = NULL;

         if(0 && b_shm_owner)
         {
            ::unlink(DATABRIDGE_SHM_NAME_POSIX);
         }
         ::close(shm_handle);
         shm_handle = -1;

         // (todo) close host event
#endif
      }
      unlockAudio();
   }

#ifdef HAVE_WINDOWS
   void sleepMillisecs(uint32_t _num) {
      ::Sleep((DWORD)_num);
   }
   bool lockChannelMutex(HANDLE _handle) {
      DWORD res = ::WaitForSingleObject(_handle, DATABRIDGE_TIMEOUT_MILLISEC);
      return (WAIT_OBJECT_0 == res);
   }
   void unlockChannelMutex(HANDLE _handle) {
      ::ReleaseMutex(_handle);
   }
#elif defined(HAVE_UNIX)
   void sleepMillisecs(uint32_t _num) {
      ::usleep(1000u * _num);
   }
   bool lockChannelMutex(int _fd) {
      ::flock(_fd, LOCK_EX);
      return true;  // (note) POSIX does not support timeouts (only via rather ugly signal hacks)
   }
   void unlockChannelMutex(int _fd) {
      ::flock(_fd, LOCK_UN);
   }
#endif

   const volatile float *getNextInputChannelChunk(void) {
      volatile float *r = NULL;

      if(lockChannelMutex(mtx_databridge_io[params.channel_idx]))
      {
         volatile databridge_channel_t *ch = &shm->channels[params.channel_idx];

         if(b_force_reset_chunk_idx)
         {
            // Consumer just connected, reset ringbuffer to sync with producer
            printf("data_bridge<consumer>: forcing ringbuffer reset after connect() or param change\n");
            b_force_reset_chunk_idx = false;
            ch->next_read_chunk_idx = 0u;
            ch->num_chunks_avail    = 0u;
         }

#if 0
         if(0u == ch->num_chunks_avail)
         {
            unlockChannelMutex(mtx_databridge_io[params.channel_idx]);
            // Wake up databridge-only host (optional)
            raiseHostEvent();
            // Wait a bit (should use an event..)
            sleepMillisecs(2u);
            lockChannelMutex(mtx_databridge_io[params.channel_idx]);
            // Now retry
         }
#endif

         if(ch->num_chunks_avail >= 1u)
         {
            uint32_t chunkIdx = ch->next_read_chunk_idx;
            ch->next_read_chunk_idx = (ch->next_read_chunk_idx + 1u) % databridge_channel_t::NUM_CHUNKS;
            ch->num_chunks_avail--;
            r = &ch->sample_data[(databridge_channel_t::CHUNK_NUM_FRAMES*2u/*stereo*/) * chunkIdx];
         }
         else
         {
            // No chunks available for reading :(
            num_underruns++;
         }

         // Wake up databridge-only host (optional)
         raiseHostEvent();
         
         // Allow producer to add more sample chunks
         unlockChannelMutex(mtx_databridge_io[params.channel_idx]);
      }

      return r;
   }

   volatile float *lockNextOutputChannelChunk(void) {
      volatile float *r = NULL;

      if(lockChannelMutex(mtx_databridge_io[params.channel_idx]))
      {
         volatile databridge_channel_t *ch = &shm->channels[params.channel_idx];

         if(ch->num_chunks_avail < databridge_channel_t::NUM_CHUNKS)
         {
            uint32_t chunkIdx = (ch->next_read_chunk_idx + ch->num_chunks_avail) % databridge_channel_t::NUM_CHUNKS;
            if(0u == (debug_output_frame_count++ & 1023u))
            { 
               printf("data_bridge: prepare out chunk_idx=%u prev numAvail=%u\n", chunkIdx, ch->num_chunks_avail);
            }
            ch->num_chunks_avail++;
            r = &ch->sample_data[ (databridge_channel_t::CHUNK_NUM_FRAMES*2u/*stereo*/) * chunkIdx];
            b_unlock_io_mutex = true;
            // (note) will be unlocked when chunk is complete OR channel is switched OR iomode changes
         }
         else
         {
            // If a tree falls in the forest and no one is there, does it still make a sound?
            //  (in this case: nope.)
            //  (next connecting consumer will get some stale samples for a short while, though)
            printf("data_bridge: discarding output chunk, all %u chunks in use\n", databridge_channel_t::NUM_CHUNKS);
            unlockChannelMutex(mtx_databridge_io[params.channel_idx]);
         }
      }
      
      return r;
   }

   void setParamIOMode(bool _bInput) {
      lockAudio();

      if(_bInput != params.b_input)
      {
         if(!params.b_input && b_unlock_io_mutex)
         {
            unlockChannelMutex(mtx_databridge_io[params.channel_idx]);
         }

         sample_ptr.any    = NULL;
         chunk_frames_left = 0u;
         b_unlock_io_mutex = false;

         params.b_input = _bInput;
         b_force_reset_chunk_idx = _bInput;
      }

      unlockAudio();
   }

   void setParamChannelIdx(uint32_t _channelIdx) {
      lockAudio();

      uint32_t newChannelIdx = _channelIdx % databridge_t::NUM_CHANNELS;

      if(params.channel_idx != newChannelIdx)
      {
         if(!params.b_input && b_unlock_io_mutex)
         {
            unlockChannelMutex(mtx_databridge_io[params.channel_idx]);
         }

         sample_ptr.any    = NULL;
         chunk_frames_left = 0u;
         b_unlock_io_mutex = false;
         
         params.channel_idx = newChannelIdx;

         b_force_reset_chunk_idx = params.b_input;
      }

      unlockAudio();
   }

   void raiseHostEvent(void) {
#ifdef HAVE_WINDOWS
      if(NULL != shm)
      {
         ::SetEvent(host_ev);
      }
#elif defined(HAVE_UNIX)
      // (todo) signal event
#endif
   }

   uint32_t getDatabridgeNumChunksAvail(void) {
      uint32_t r = 0u;
      if(NULL != shm)
      {
         lockAudio();
         lockChannelMutex(mtx_databridge_io[params.channel_idx]);
         r = shm->channels[params.channel_idx].num_chunks_avail;
         unlockChannelMutex(mtx_databridge_io[params.channel_idx]);
         unlockAudio();
      }
      return r;
   }

private:
   // the host callback (a function pointer)
   VSTHostCallback _vstHostCallback;

   // the actual structure required by the host
   VSTPlugin _vstPlugin;
};


/*******************************************
 * Callbacks: Host -> Plugin
 *
 * Defined here because they are used in the rest of the code later
 */

/**
 * This is the callback that will be called to process the samples in the case of single precision. This is where the
 * meat of the logic happens!
 *
 * @param vstPlugin the object returned by VSTPluginMain
 * @param inputs an array of array of input samples. You read from it. First dimension is for inputs, second dimension is for samples: inputs[numInputs][sampleFrames]
 * @param outputs an array of array of output samples. You write to it. First dimension is for outputs, second dimension is for samples: outputs[numOuputs][sampleFrames]
 * @param sampleFrames the number of samples (second dimension in both arrays)
 */
void VSTPluginProcessReplacingFloat32(VSTPlugin *vstPlugin,
                                      float    **inputs,
                                      float    **outputs,
                                      VstInt32   sampleFrames
                                      ) {
   // we can get a hold to our C++ class since we stored it in the `object` field (see constructor)
   VSTPluginWrapper *wrapper = static_cast<VSTPluginWrapper *>(vstPlugin->object);

   wrapper->lockAudio();

   if(wrapper->b_processing)
   {
      if(wrapper->params.b_input)
      {
         // Copy shm input to VST output
         float *dl = outputs[0];
         float *dr = outputs[1];
         
         if((NULL != dl) && (NULL != dr))  // paranoia
         {
            uint32_t outSamplesLeft = uint32_t(sampleFrames);

            while(outSamplesLeft > 0u)
            {
               const volatile float *s;

               if(0u == wrapper->chunk_frames_left)
               {
                  // Need moar data
                  wrapper->sample_ptr.in = s = wrapper->getNextInputChannelChunk();

                  if(NULL != s)
                  {
                     wrapper->chunk_frames_left = databridge_channel_t::CHUNK_NUM_FRAMES;
                  }
               }
               else
               {
                  // Continue reading from current chunk
                  s = wrapper->sample_ptr.in;
               }

               if(NULL != s)
               {
                  // Copy samples
                  uint32_t numFrames = (outSamplesLeft < wrapper->chunk_frames_left) ? outSamplesLeft : wrapper->chunk_frames_left;

                  for(uint32_t k = 0u; k < numFrames; k++)
                  {
                     dl[k] = *s++;
                     dr[k] = *s++;
                  }

                  dl += numFrames;
                  dr += numFrames;
                  outSamplesLeft -= numFrames;
                  wrapper->chunk_frames_left -= numFrames;
                  wrapper->sample_ptr.in = s;
               }
               else
               {
                  // No more samples available, fill with 0
                  for(uint32_t k = 0u; k < outSamplesLeft; k++)
                  {
                     dl[k] = 0.0f;
                     dr[k] = 0.0f;
                  }
                  outSamplesLeft = 0u;
               }

            } // while outSamplesLeft > 0u

         } // if have output buffers
      }
      else
      {
         // Copy VST input to shm output
         const float *sl = inputs[0];
         const float *sr = inputs[1];
         
         if((NULL != sl) && (NULL != sr))  // paranoia
         {
            uint32_t inSamplesLeft = uint32_t(sampleFrames);

            while(inSamplesLeft > 0u)
            {
               volatile float *d;

               if(0u == wrapper->chunk_frames_left)
               {
                  // Have moar data
                  wrapper->sample_ptr.out = d = wrapper->lockNextOutputChannelChunk();

                  if(NULL != d)
                  {
                     wrapper->chunk_frames_left = databridge_channel_t::CHUNK_NUM_FRAMES;
                  }
                  // else: no consumer and ringbuffer is full => stop writing to shm
               }
               else
               {
                  // Continue reading from current chunk
                  d = wrapper->sample_ptr.out;
               }

               if(NULL != d)
               {
                  // Copy samples
                  uint32_t numFrames = (inSamplesLeft < wrapper->chunk_frames_left) ? inSamplesLeft : wrapper->chunk_frames_left;

                  for(uint32_t k = 0u; k < numFrames; k++)
                  {
                     *d++ = sl[k];
                     *d++ = sr[k];
                  }

                  sl += numFrames;
                  sr += numFrames;
                  inSamplesLeft -= numFrames;
                  wrapper->chunk_frames_left -= numFrames;
                  wrapper->sample_ptr.out = d;

                  if(0u == wrapper->chunk_frames_left)
                  {
                     // Chunk complete, make data available to consumer
                     wrapper->unlockChannelMutex(wrapper->mtx_databridge_io[wrapper->params.channel_idx]);
                  }

               } // if d
               else
               {
                  // Something went terribly wrong, have no place to store the samples => ABORT, ABORT!
                  inSamplesLeft = 0u;
               }
            }
            // else: no consumer
         } // if sl && sr

         // Since we announced this as an effect with both inputs+outputs, copy the input to the output
         {
            const float *sl = inputs[0];
            const float *sr = inputs[1];

            float *dl = outputs[0];
            float *dr = outputs[1];
         
            if((NULL != dl) && (NULL != dr))  // paranoia
            {
               if((NULL != sl) && (NULL != sr))  // paranoia
               {
                  for(uint32_t k = 0u; k < uint32_t(sampleFrames); k++)
                  {
                     dl[k] = sl[k];
                     dr[k] = sr[k];
                  }
               }
            }
         }

      } // input or output
   } // if b_processing

   wrapper->unlockAudio();
}


#if 0
/**
 * This is the callback that will be called to process the samples in the case of double precision. This is where the
 * meat of the logic happens!
 *
 * @param vstPlugin the object returned by VSTPluginMain
 * @param inputs an array of array of input samples. You read from it. First dimension is for inputs, second dimension is for samples: inputs[numInputs][sampleFrames]
 * @param outputs an array of array of output samples. You write to it. First dimension is for outputs, second dimension is for samples: outputs[numOuputs][sampleFrames]
 * @param sampleFrames the number of samples (second dimension in both arrays)
 */
void VSTPluginProcessReplacingFloat64(VSTPlugin *vstPlugin, 
                                      double   **inputs, 
                                      double   **outputs, 
                                      VstInt32   sampleFrames
                                      ) {
   // we can get a hold to our C++ class since we stored it in the `object` field (see constructor)
   VSTPluginWrapper *wrapper = static_cast<VSTPluginWrapper *>(vstPlugin->object);

   wrapper->lockAudio();

   if(wrapper->b_processing)
   {
      // code speaks for itself: for each input (2 when stereo input), iterating over every sample and writing the
      // result in the outputs array after multiplying by 0.5 (which result in a 3dB attenuation of the sound)
      for(int i = 0; i < wrapper->getNumInputs(); i++)
      {
         auto inputSamples = inputs[i];
         auto outputSamples = outputs[i];

         for(int j = 0; j < sampleFrames; j++)
         {
            outputSamples[j] = inputSamples[j] * 0.5;
         }
      }
   }

   wrapper->unlockAudio();
}
#endif // 0


/**
 * This is the plugin called by the host to communicate with the plugin, mainly to request information (like the
 * vendor string, the plugin category...) or communicate state/changes (like open/close, frame rate...)
 *
 * @param vstPlugin the object returned by VSTPluginMain
 * @param opCode defined in aeffect.h/AEffectOpcodes and which continues in aeffectx.h/AEffectXOpcodes for a grand
 *        total of 79 of them! Only a few of them are implemented in this small plugin.
 * @param index depend on the opcode
 * @param value depend on the opcode
 * @param ptr depend on the opcode
 * @param opt depend on the opcode
 * @return depend on the opcode (0 is ok when you don't implement an opcode...)
 */
VstIntPtr VSTPluginDispatcher(VSTPlugin *vstPlugin,
                              VstInt32   opCode,
                              VstInt32   index,
                              VstIntPtr  value,
                              void      *ptr,
                              float      opt
                              ) {
   // printf("data_bridge: called VSTPluginDispatcher(%d)\n", opCode);

   VstIntPtr r = 0;

   // we can get a hold to our C++ class since we stored it in the `object` field (see constructor)
   VSTPluginWrapper *wrapper = static_cast<VSTPluginWrapper *>(vstPlugin->object);

   // see aeffect.h/AEffectOpcodes and aeffectx.h/AEffectXOpcodes for details on all of them
   switch(opCode)
   {
      case effGetPlugCategory:
         // request for the category of the plugin: in this case it is an effect since it is modifying the input (as opposed
         // to generating sound)
         return kPlugCategEffect;

      case effOpen:
         // called by the host after it has obtained the effect instance (but _not_ during plugin scans)
         //  (note) any heavy-lifting init code should go here
         ::printf("data_bridge<dispatcher>: effOpen\n");
         wrapper->connect();
         r = 1;
         break;

      case effClose:
         // called by the host when the plugin was called... time to reclaim memory!
         wrapper->disconnect();
         // (note) hosts usually call effStopProcess before effClose
         delete wrapper;
         break;

      case effGetVendorString:
         // request for the vendor string (usually used in the UI for plugin grouping)
         ::strncpy(static_cast<char *>(ptr), "pongasoft and bsp", kVstMaxVendorStrLen);
         r = 1;
         break;

      case effGetVendorVersion:
         // request for the version
         return PLUGIN_VERSION;

      case effGetEffectName:
         ::strncpy((char*)ptr, "data_bridge", kVstMaxEffectNameLen);
         r = 1;
         break;

      case effGetProductString:
         ::strncpy((char*)ptr, "data_bridge v0.1", kVstMaxProductStrLen);
         r = 1;
         break;

      case effGetNumMidiInputChannels:
         r = 0;
         break;

      case effGetNumMidiOutputChannels:
         r = 0;
         break;

      case effGetInputProperties:
         {
            VstPinProperties *pin = (VstPinProperties*)ptr;
            ::snprintf(pin->label, kVstMaxLabelLen, "Input #%d", index);
            pin->flags           = kVstPinIsActive | ((0 == (index & 1)) ? kVstPinIsStereo : 0);
            pin->arrangementType = ((0 == (index & 1)) ? kSpeakerArrStereo : kSpeakerArrMono);
            ::snprintf(pin->shortLabel, kVstMaxShortLabelLen, "in%d", index);
            memset((void*)pin->future, 0, 48);
            r = 1;
         }
         break;

      case effGetOutputProperties:
         {
            VstPinProperties *pin = (VstPinProperties*)ptr;
            ::snprintf(pin->label, kVstMaxLabelLen, "Output #%d", index);
            pin->flags           = kVstPinIsActive | ((0 == (index & 1)) ? kVstPinIsStereo : 0);
            pin->arrangementType = ((0 == (index & 1)) ? kSpeakerArrStereo : kSpeakerArrMono);
            ::snprintf(pin->shortLabel, kVstMaxShortLabelLen, "out%d", index);
            memset((void*)pin->future, 0, 48);
            r = 1;
         }
         break;

      case effSetSampleRate:
         r = wrapper->setSampleRate(opt) ? 1 : 0;
         break;

      case effSetBlockSize:
         r = wrapper->setBlockSize(uint32_t(value)) ? 1 : 0;
         break;

      case effCanDo:
         // ptr:
         // "sendVstEvents"
         // "sendVstMidiEvent"
         // "sendVstTimeInfo"
         // "receiveVstEvents"
         // "receiveVstMidiEvent"
         // "receiveVstTimeInfo"
         // "offline"
         // "plugAsChannelInsert"
         // "plugAsSend"
         // "mixDryWet"
         // "noRealTime"
         // "multipass"
         // "metapass"
         // "1in1out"
         // "1in2out"
         // "2in1out"
         // "2in2out"
         // "2in4out"
         // "4in2out"
         // "4in4out"
         // "4in8out"
         // "8in4out"
         // "8in8out"
         // "midiProgramNames"
         // "conformsToWindowRules"
         r = 0;
         break;

      case effGetProgramName:
         ::snprintf((char*)ptr, kVstMaxProgNameLen, "default");
         r = 1;
         break;

      case effSetProgramName:
         r = 0;
         break;

      case effGetProgramNameIndexed:
         r = 0;
         break;

      case effGetParamName:
      case effGetParamLabel:
         // kVstMaxParamStrLen, much longer in other plugins
         switch(index)
         {
            default:
               r = 0;
               break;

            case 0:
               ::snprintf((char*)ptr, kVstMaxParamStrLen, "b_input");
               r = 1;
               break;

            case 1:
               ::snprintf((char*)ptr, kVstMaxParamStrLen, "channel_idx");
               r = 1;
               break;
         }
         break;

      case effGetParameterProperties:
         r = 0;
         break;

      case effGetChunk:
         // Query bank (index=0) or program (index=1) state
         //  value: 0
         //    ptr: buffer address
         //      r: buffer size
         r = 0;
         break;

      case effSetChunk:
         // Restore bank (index=0) or program (index=1) state
         //  value: buffer size
         //    ptr: buffer address
         //      r: 1
         if(0 == index)
         {
            r = wrapper->setBankChunk(size_t(value), (uint8_t*)ptr) ? 1 : 0;
         }
         else
         {
            r = wrapper->setProgramChunk(size_t(value), (uint8_t*)ptr) ? 1 : 0;
         }
         break;

      case effShellGetNextPlugin:
         // For shell plugins (e.g. Waves), returns next sub-plugin UID (or 0)
         //  (note) plugin uses audioMasterCurrentId while it's being instantiated to query the currently selected sub-plugin
         //          if the host returns 0, it will then call effShellGetNextPlugin to enumerate the sub-plugins
         //  ptr: effect name string ptr (filled out by the plugin)
         r = 0;
         break;

      case effMainsChanged:
         // value = 0=suspend, 1=resume
         wrapper->setEnableProcessingActive((value > 0) ? true : false);
         r = 1;
         break;

      case effStartProcess:
         wrapper->setEnableProcessingActive(true);
         r = 1;
         break;

      case effStopProcess:
         wrapper->setEnableProcessingActive(false);
         r = 1;
         break;

      case effProcessEvents:
         // ptr: VstEvents*
         {
            VstEvents *events = (VstEvents*)ptr;
            //printf("data_bridge:effProcessEvents: recvd %d events", events->numEvents);
            VstEvent**evAddr = &events->events[0];

            for(uint32_t evIdx = 0u; evIdx < uint32_t(events->numEvents); evIdx++, evAddr++)
            {
               VstEvent *ev = *evAddr;

               if(NULL != ev)  // paranoia
               {
                  printf("data_bridge:effProcessEvents: ev[%u].byteSize    = %u\n", evIdx, uint32_t(ev->byteSize));  // sizeof(VstMidiEvent) = 32
                  printf("data_bridge:effProcessEvents: ev[%u].deltaFrames = %u\n", evIdx, uint32_t(ev->deltaFrames));

                  switch(ev->type)
                  {
                     default:
                     //case kVstAudioType:      // deprecated
                     //case kVstVideoType:      // deprecated
                     //case kVstParameterType:  // deprecated
                     //case kVstTriggerType:    // deprecated
                        break;

                     case kVstMidiType:
                        // (note) ev->data stores the actual payload (up to 16 bytes)
                        // (note) e.g. 0x90 0x30 0x7F for a C-4 note-on on channel 1 with velocity 127
                        // (note) don't forget to use a mutex (lockAudio(), unlockAudio()) when modifying the audio processor state!
                        {
                           VstMidiEvent *mev = (VstMidiEvent *)ev;
                           printf("data_bridge:effProcessEvents<midi>: ev[%u].noteLength      = %u\n", evIdx, uint32_t(mev->noteLength));  // #frames
                           printf("data_bridge:effProcessEvents<midi>: ev[%u].noteOffset      = %u\n", evIdx, uint32_t(mev->noteOffset));  // #frames
                           printf("data_bridge:effProcessEvents<midi>: ev[%u].midiData        = %02x %02x %02x %02x\n", evIdx, uint8_t(mev->midiData[0]), uint8_t(mev->midiData[1]), uint8_t(mev->midiData[2]), uint8_t(mev->midiData[3]));
                           printf("data_bridge:effProcessEvents<midi>: ev[%u].detune          = %d\n", evIdx, mev->detune); // -64..63
                           printf("data_bridge:effProcessEvents<midi>: ev[%u].noteOffVelocity = %d\n", evIdx, mev->noteOffVelocity); // 0..127
                        }
                        break;

                     case kVstSysExType:
                     {
                        VstMidiSysexEvent *xev = (VstMidiSysexEvent*)ev;

                        printf("data_bridge:effProcessEvents<syx>: ev[%u].dumpBytes = %u\n", evIdx, uint32_t(xev->dumpBytes));  // size
                        printf("data_bridge:effProcessEvents<syx>: ev[%u].sysexDump = %p\n", evIdx, xev->sysexDump);            // buffer addr

                        // (note) don't forget to use a mutex (lockAudio(), unlockAudio()) when modifying the audio processor state!
                     }
                     break;
                  }
               } // if ev
            } // loop events
         }
         break;

#if 0
      case effIdle:
         // Periodic idle call (from UI thread), e.g. at 20ms intervals (depending on host)
         //  (note) deprecated in vst2.4 (but some plugins still rely on this)
         r = 1;
         break;
#endif

#if 0
      case effEditIdle:
         // deprecated in vst2.4
         break;
#endif

      case effEditGetRect:
         // Query editor window geometry
         // ptr: ERect* (on Windows)
         if(NULL != ptr) // yeah, this should never be NULL
         {
            // ...
         }
         else
         {
            r = 0;
         }
         break;

#if 0
      case effEditTop:
         // deprecated in vst2.4
         r = 0;
         break;
#endif

      case effEditOpen:
         // Show editor window
         // ptr: native window handle (hWnd on Windows)
         r = 0;
         break;

      case effEditClose:
         // Hide editor window
         r = 0;
         break;

      case effDataBridgeGetNumChunksAvail:
         // proprietary data_bridge extension, used by databridge-only host
         r = wrapper->getDatabridgeNumChunksAvail();
         break;

      default:
         // ignoring all other opcodes
         printf("data_bridge:dispatcher: unhandled opCode %d [ignored] \n", opCode);
         break;

   }

   return r;
}


/**
 * Set parameter setting
 */
void VSTPluginSetParameter(VSTPlugin *vstPlugin,
                           VstInt32   index,
                           float      parameter
                           ) {
   printf("data_bridge: called VSTPluginSetParameter(%d, %f)\n", index, parameter);
   // we can get a hold to our C++ class since we stored it in the `object` field (see constructor)
   VSTPluginWrapper *wrapper = static_cast<VSTPluginWrapper *>(vstPlugin->object);

   switch(index)
   {
      default:
         break;

      case 0: // iomode / "b_input"
         wrapper->setParamIOMode(uint32_t(parameter + 0.5f));
         break;

      case 1: // channelidx / "channel_idx"
         wrapper->setParamChannelIdx(uint32_t(parameter));
         break;
   }
}


/**
 * Query parameter
 */
float VSTPluginGetParameter(VSTPlugin *vstPlugin,
                            VstInt32   index
                            ) {
   printf("data_bridge: called VSTPluginGetParameter(%d)\n", index);
   // we can get a hold to our C++ class since we stored it in the `object` field (see constructor)
   VSTPluginWrapper *wrapper = static_cast<VSTPluginWrapper *>(vstPlugin->object);

   float r = 0.0f;

   switch(index)
   {
      default:
         break;

      case 0: // iomode / "b_input"
         r = wrapper->params.b_input ? 1.0f : 0.0f;
         break;

      case 1: // channelidx / "channel_idx"
         r = float(wrapper->params.channel_idx);
         break;
   }

   return r;
}


/**
 * Main constructor for our C++ class
 */
VSTPluginWrapper::VSTPluginWrapper(audioMasterCallback vstHostCallback,
                                   VstInt32 vendorUniqueID,
                                   VstInt32 vendorVersion,
                                   VstInt32 numParams,
                                   VstInt32 numPrograms,
                                   VstInt32 numInputs,
                                   VstInt32 numOutputs
                                   ) : _vstHostCallback(vstHostCallback)
{
   // Make sure that the memory is properly initialized
   memset(&_vstPlugin, 0, sizeof(_vstPlugin));

   // this field must be set with this constant...
   _vstPlugin.magic = kEffectMagic;

   // storing this object into the VSTPlugin so that it can be retrieved when called back (see callbacks for use)
   _vstPlugin.object = this;

   // specifying that we handle both single and NOT double precision (there are other flags see aeffect.h/VstAEffectFlags)
   _vstPlugin.flags = effFlagsCanReplacing | (effFlagsCanDoubleReplacing & 0);

   // initializing the plugin with the various values
   _vstPlugin.uniqueID    = vendorUniqueID;
   _vstPlugin.version     = vendorVersion;
   _vstPlugin.numParams   = numParams;
   _vstPlugin.numPrograms = numPrograms;
   _vstPlugin.numInputs   = numInputs;
   _vstPlugin.numOutputs  = numOutputs;

   // setting the callbacks to the previously defined functions
   _vstPlugin.dispatcher             = &VSTPluginDispatcher;
   _vstPlugin.getParameter           = &VSTPluginGetParameter;
   _vstPlugin.setParameter           = &VSTPluginSetParameter;
   _vstPlugin.processReplacing       = &VSTPluginProcessReplacingFloat32;
   _vstPlugin.processDoubleReplacing = NULL;//&VSTPluginProcessReplacingFloat64;

   // report latency
   _vstPlugin.initialDelay = databridge_channel_t::CHUNK_NUM_FRAMES;

   sample_rate  = 44100.0f;
   block_size   = 64u;
   b_processing = true;

   b_shm_owner = false;
   shm         = NULL;
#ifdef HAVE_WINDOWS
   shm_handle  = NULL;
#elif defined(HAVE_UNIX)
   shm_handle  = -1;
#endif

   params.b_input     = true;
   params.channel_idx = 0u;
   b_force_reset_chunk_idx = true;

   num_underruns = 0u;

   chunk_frames_left  = 0u;
   sample_ptr.any     = NULL;
   b_unlock_io_mutex  = false;

   debug_output_frame_count = 0u;
}

/**
 * Destructor called when the plugin is closed (see VSTPluginDispatcher with effClose opCode). In this very simply plugin
 * there is nothing to do but in general the memory that gets allocated MUST be freed here otherwise there might be a
 * memory leak which may end up slowing down and/or crashing the host
 */
VSTPluginWrapper::~VSTPluginWrapper() {
   disconnect();
}


/**
 * Implementation of the main entry point of the plugin
 */
VST_EXPORT VSTPlugin *VSTPluginMain(VSTHostCallback vstHostCallback) {
   printf("data_bridge: called VSTPluginMain... \n");

   // simply create our plugin C++ class
   VSTPluginWrapper *plugin =
      new VSTPluginWrapper(vstHostCallback,
                           CCONST('u', 's', 'a', 'n'), // registered with Steinberg (http://service.steinberg.de/databases/plugin.nsf/plugIn?openForm)
                           PLUGIN_VERSION, // version
                           2,    // two params (iomode and channelidx)
                           0,    // no programs
                           2,    // 2 inputs
                           2     // 2 outputs
                           );

   // return the plugin per the contract of the API
   return plugin->getVSTPlugin();
}
