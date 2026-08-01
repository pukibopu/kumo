#include <kumo/agent/http_provider.h>

#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <expected>

namespace kumo::agent {

// Thin synchronous shim over NSURLSession (ADR 0005/0034): no retries, no
// classification — that policy lives in HttpLLMProvider. The semaphore wait is
// sliced so an aborting session can cancel the in-flight task.
HttpTransport makeUrlSessionTransport() {
    return [](const HttpRequest& request, std::chrono::seconds timeout,
              const std::atomic<bool>& abort) -> std::expected<HttpResponse, TransportError> {
        @autoreleasepool {
            NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
            if (url == nil) {
                return std::unexpected(
                    TransportError{.kind = TransportError::Kind::ConnectionFailed,
                                   .message = "invalid url: " + request.url});
            }
            NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
            urlRequest.HTTPMethod = [NSString stringWithUTF8String:request.method.c_str()];
            for (const auto& [name, value] : request.headers) {
                [urlRequest setValue:[NSString stringWithUTF8String:value.c_str()]
                    forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
            }
            if (!request.body.empty()) {
                urlRequest.HTTPBody = [NSData dataWithBytes:request.body.data()
                                                     length:request.body.size()];
            }

            NSURLSessionConfiguration* configuration =
                [NSURLSessionConfiguration ephemeralSessionConfiguration];
            configuration.timeoutIntervalForRequest = static_cast<double>(timeout.count());
            configuration.timeoutIntervalForResource = static_cast<double>(timeout.count());
            NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration];

            __block HttpResponse response;
            __block TransportError error;
            __block bool failed = false;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            NSURLSessionDataTask* task = [session
                dataTaskWithRequest:urlRequest
                  completionHandler:^(NSData* data, NSURLResponse* urlResponse, NSError* nsError) {
                    if (nsError != nil) {
                        failed = true;
                        error.kind = nsError.code == NSURLErrorTimedOut
                                         ? TransportError::Kind::Timeout
                                         : (nsError.code == NSURLErrorCancelled
                                                ? TransportError::Kind::Cancelled
                                                : TransportError::Kind::ConnectionFailed);
                        error.message = nsError.localizedDescription.UTF8String;
                    } else {
                        auto* http = (NSHTTPURLResponse*)urlResponse;
                        response.status = static_cast<int>(http.statusCode);
                        if (data != nil) {
                            response.body.assign(static_cast<const char*>(data.bytes), data.length);
                        }
                        // Only header the retry policy needs (ADR 0030 update);
                        // field lookup is case-insensitive.
                        if (NSString* retryAfter = [http valueForHTTPHeaderField:@"Retry-After"];
                            retryAfter != nil) {
                            response.retryAfter = std::string(retryAfter.UTF8String);
                        }
                    }
                    dispatch_semaphore_signal(done);
                  }];
            [task resume];
            while (dispatch_semaphore_wait(
                       done, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0) {
                if (abort.load()) {
                    [task cancel]; // completion handler fires with NSURLErrorCancelled
                }
            }
            [session finishTasksAndInvalidate];
            if (failed) {
                return std::unexpected(error);
            }
            return response;
        }
    };
}

} // namespace kumo::agent
