/**
 * File              : main.m
 * Author            : Igor V. Sementsov <ig.kuzm@gmail.com>
 * Date              : 09.08.2023
 * Last Modified Date: 10.08.2023
 * Last Modified By  : Igor V. Sementsov <ig.kuzm@gmail.com>
 */

#import <UIKit/UIKit.h>
#import "AppDelegate.h"
#include <signal.h>

int main(int argc, char *argv[]) {
    
  signal(SIGPIPE, SIG_IGN);
  @autoreleasepool {
    return 
			UIApplicationMain(
					argc, argv, nil, 
					NSStringFromClass([AppDelegate class]));
  }
}

// vim:ft=objc
