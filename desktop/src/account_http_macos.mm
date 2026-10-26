#include "account.h"
#import <Foundation/Foundation.h>

@interface FFAccountDelegate : NSObject <NSURLSessionDataDelegate>
@property(nonatomic, strong) NSMutableData* bytes;
@property(nonatomic) NSInteger status;
@property(nonatomic) BOOL failed;
@property(nonatomic, strong) dispatch_semaphore_t finished;
@end
@implementation FFAccountDelegate
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request
 completionHandler:(void (^)(NSURLRequest*))completionHandler {
    (void)session; (void)task; (void)response; (void)request;
    completionHandler(nil);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
    (void)session;
    if (self.bytes.length + data.length > fileflow::desktop::account::kMaxResponseBytes) {
        self.failed = YES; [task cancel]; return;
    }
    [self.bytes appendData:data];
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;
    self.failed = self.failed || error != nil;
    if ([task.response isKindOfClass:[NSHTTPURLResponse class]]) {
        id response = task.response; self.status = [response statusCode];
    }
    dispatch_semaphore_signal(self.finished);
}
@end

namespace fileflow::desktop::account {
HttpResponse Request(std::string_view path, std::string_view body, std::string_view bearer) {
    @autoreleasepool {
        if (!ValidRequest(path, body, bearer)) return {};
        const std::string target = Origin() + std::string(path);
        NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:target.c_str()]];
        if (![url.scheme isEqualToString:@"https"]) return {};
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
        request.timeoutInterval = 10;
        request.HTTPMethod = body.empty() ? @"GET" : @"POST";
        [request setValue:@"application/json" forHTTPHeaderField:@"Accept"];
        if (!body.empty()) {
            [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
            request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
        }
        if (!bearer.empty()) {
            const std::string value = "Bearer " + std::string(bearer);
            [request setValue:[NSString stringWithUTF8String:value.c_str()] forHTTPHeaderField:@"Authorization"];
        }
        FFAccountDelegate* delegate = [[FFAccountDelegate alloc] init];
        delegate.bytes = [NSMutableData data]; delegate.finished = dispatch_semaphore_create(0);
        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = 10; cfg.timeoutIntervalForResource = 10;
        cfg.HTTPShouldSetCookies = NO; cfg.HTTPCookieStorage = nil; cfg.URLCache = nil;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg delegate:delegate delegateQueue:nil];
        [[session dataTaskWithRequest:request] resume];
        if (dispatch_semaphore_wait(delegate.finished, dispatch_time(DISPATCH_TIME_NOW, 11 * NSEC_PER_SEC)) != 0) {
            [session invalidateAndCancel]; return {};
        }
        HttpResponse result;
        if (!delegate.failed) {
            result.status = static_cast<int>(delegate.status);
            result.body.assign(static_cast<const char*>(delegate.bytes.bytes), delegate.bytes.length);
        }
        [session finishTasksAndInvalidate];
        return result;
    }
}
}  // namespace fileflow::desktop::account
