#include "UIKit/UIKit.h"
#import <UIKit/UIKit.h>
#import "TGMessage.h"

@interface ChatViewCell : UITableViewCell
{
}
@property (strong) TGMessage *message;
@property (strong) UIImageView *avatarView;
@property (strong) UIImageView *photoView;
@property (strong) UITextView *textView;
@property (strong) UILabel *fromLabael;
@property (strong) UILabel *timeLabael;
@property (strong) UILabel *deliveredLabael;
@property (nonatomic, assign) CGFloat photoHeight;
@property (nonatomic, assign) CGFloat textHeight;

-(void)setMessage:(TGMessage *)message;
@end
// vim:ft=objc
