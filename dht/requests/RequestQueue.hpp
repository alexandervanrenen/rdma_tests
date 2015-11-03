//---------------------------------------------------------------------------
// (c) 2015 Wolf Roediger <roediger@in.tum.de>
// Technische Universitaet Muenchen
// Institut fuer Informatik, Lehrstuhl III
// Boltzmannstr. 3
// 85748 Garching
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//---------------------------------------------------------------------------
#pragma once
//---------------------------------------------------------------------------
#include <memory>
#include <array>
#include <vector>
#include <zmq.hpp>
//---------------------------------------------------------------------------
#include "rdma/Network.hpp"
#include "rdma/MemoryRegion.hpp"
#include "rdma/WorkRequest.hpp"
#include "rdma/QueuePair.hpp"
#include "rdma/CompletionQueuePair.hpp"
#include "util/NotAssignable.hpp"
#include "dht/Common.hpp"
//---------------------------------------------------------------------------
struct ibv_send_wr;
//---------------------------------------------------------------------------
namespace dht { // Distributed Hash Table
//---------------------------------------------------------------------------
struct HashTableNetworkLayout;
struct HashTableServer;
//---------------------------------------------------------------------------
enum class RequestStatus : public uint8_t {
   FINISHED, SEND_AGAIN
};
//---------------------------------------------------------------------------
struct Request : public util::NotAssignable {
   virtual RequestStatus onCompleted() = 0;
   virtual rdma::WorkRequest &getRequest() = 0;
   virtual ~RequestStatus() { }
};
using namespace std;
//---------------------------------------------------------------------------
struct InsertRequest : public Request {
   rdma::AtomicCompareAndSwapWorkRequest workRequest;

   BucketLocator oldBucketLocator;
   rdma::MemoryRegion oldBucketLocatorMR;

   Bucket *bucket;
   Entry *entry;

   InsertRequest(rdma::Network &network)
           : oldBucketLocatorMR(&oldBucketLocator, sizeof(oldBucketLocator), network.getProtectionDomain(), rdma::MemoryRegion::Permission::All)
   {
   }

   ~InsertRequest() { }

   virtual RequestStatus onCompleted()
   {
      if (oldBucketLocator.data != workRequest.getCompareValue()) {
         workRequest.setCompareValue(oldBucketLocator.data);
         return RequestStatus::SEND_AGAIN;
      }

      bucket->next.data = oldBucketLocator.data;
      bucket->entry = *entry;

      return RequestStatus::FINISHED;
   }

   void init(Entry *entry, Bucket *bucket, rdma::RemoteMemoryRegion &remoteTarget, const BucketLocator &localBucketLocation)
   {
      this->entry = entry;
      this->bucket = bucket;

      // Init work request
      workRequest.setId((uintptr_t) this);
      workRequest.setLocalAddress(oldBucketLocatorMR);
      workRequest.setCompareValue(0);
      workRequest.setRemoteAddress(remoteTarget);
      workRequest.setSwapValue(localBucketLocation.data);
   }

   virtual rdma::WorkRequest &getRequest() { return workRequest; }
};
//---------------------------------------------------------------------------
struct DummyRequest : public Request {
   rdma::ReadWorkRequest workRequest;

   uint64_t memory;
   rdma::MemoryRegion memoryMR;

   DummyRequest(rdma::Network &network, rdma::RemoteMemoryRegion &remoteMemoryRegion)
           : memoryMR(&memory, sizeof(memory), network.getProtectionDomain(), rdma::MemoryRegion::Permission::All)
   {
      workRequest.setCompletion(true);
      workRequest.setLocalAddress(memoryMR);
      workRequest.setRemoteAddress(remoteMemoryRegion);
   }

   ~InsertRequest() { }

   virtual RequestStatus onCompleted()
   {
      return RequestStatus::FINISHED;
   }

   virtual rdma::WorkRequest &getRequest() { return workRequest; }
};
//---------------------------------------------------------------------------
class RequestQueue : public util::NotAssignable {
public:
   RequestQueue(uint bundleSize, uint bundleCount, rdma::QueuePair &queuePair, DummyRequest &dummyRequest)
           : queuePair(queuePair)
             , bundles(bundleCount, Bundle{vector<Request *>(bundleSize)})
             , bundleSize(bundleSize)
             , bundleCount(bundleCount)
             , currentBundle(0)
             , nextWorkRequestInBundle(0)
             , bundleUpForCompletion(0)
             , dummyRequest(dummyRequest)
   {
   }

   ~RequestQueue() { }

   void submit(Request *request)
   {
      // Wait
      while (nextWorkRequestInBundle>=bundleSize) {
         nextWorkRequestInBundle = 0;
         currentBundle = (currentBundle + 1) % bundleCount;

         // Wait until the bundle is completed
         if (currentBundle == bundleUpForCompletion) {
            // Wait for completion event
            queuePair.getCompletionQueuePair().waitForCompletionSend();
            bundleUpForCompletion = (bundleUpForCompletion + 1) % bundleCount;

            // Notify all request of this bundle that they are completed
            for (auto iter : bundles[currentBundle].requests)
               if (iter->onCompleted() == RequestStatus::SEND_AGAIN)
                  send(iter);
         }
      }

      // Send
      send(request);
   }

   void finishAllOpenRequests()
   {
      // Find all open work request
      std::vector<Request *> openRequests;
      while (nextWorkRequestInBundle != 0 || currentBundle != bundleUpForCompletion) {
         if (nextWorkRequestInBundle == 0) {
            currentBundle = (currentBundle - 1 + bundleCount) % bundleCount;
            nextWorkRequestInBundle = bundleSize;
         } else {
            nextWorkRequestInBundle--;
            openRequests.push_back(bundles[currentBundle].requests[nextWorkRequestInBundle]);
         }
      }

      // Wait for completions
      for (int i = 0; i<openRequests.size() / bundleSize; ++i)
         queuePair.getCompletionQueuePair().waitForCompletionSend();

      // Make them finish
      while (!openRequests.empty()) {
         queuePair.postWorkRequest(dummyRequest.getRequest());
         queuePair.getCompletionQueuePair().waitForCompletionSend();

         std::vector<Request *> stillOpenRequests;
         for (auto iter : openRequests) {
            if (iter->onCompleted() == RequestStatus::SEND_AGAIN) {
               queuePair.postWorkRequest(iter->getRequest());
               stillOpenRequests.push_back(iter);
            }
         }
         swap(openRequests, stillOpenRequests);
      }
   }

private:
   void send(Request *request)
   {
      bundles[currentBundle].requests[nextWorkRequestInBundle++] = request;
      request->getRequest().setCompletion(nextWorkRequestInBundle == bundleSize);
      queuePair.postWorkRequest(request->getRequest());
   }

   struct Bundle {
      std::vector<Request *> requests;
   };

   rdma::QueuePair &queuePair;
   std::vector<Bundle> bundles;
   const uint32_t bundleSize;
   const uint32_t bundleCount;
   uint32_t currentBundle;
   uint32_t nextWorkRequestInBundle;
   uint32_t bundleUpForCompletion;
   DummyRequest &dummyRequest;
};
//---------------------------------------------------------------------------
} // End of namespace dht
//---------------------------------------------------------------------------