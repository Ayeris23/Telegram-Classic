//
//  TGOggOpusWriter.h
//  Telegram
//
//  Created by Ayeris on 6/23/26.
//  Copyright (c) 2026 Ayeris. All rights reserved.
//

#import <Foundation/Foundation.h>

@interface TGOggOpusWriter : NSObject

- (bool)begin:(NSFileHandle *)fileHandle;
- (bool)writeFrame:(uint8_t *)framePcmBytes frameByteCount:(NSUInteger)frameByteCount;
- (NSUInteger)encodedBytes;
- (NSTimeInterval)encodedDuration;

@end
