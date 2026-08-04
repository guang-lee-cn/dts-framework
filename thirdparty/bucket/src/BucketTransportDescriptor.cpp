#include "bucket/BucketTransportDescriptor.h"

#include "bucket/BucketTransport.h"

namespace bucket {

eprosima::fastdds::rtps::TransportInterface* BucketTransportDescriptor::create_transport() const
{
    return new BucketTransport(*this);
}

}  // namespace bucket
