#ifndef HX_SOUND_H
#define HX_SOUND_H

#define CHAT_INVITE 0
#define CHAT_POST 1
#define ERROR 2
#define FILE_DONE 3
#define USER_JOIN 4
#define LOGIN 5
#define MSG 6
#define NEWS_POST 7
#define USER_PART 8


extern struct hx_sounds hxsnd;
extern void play_sound(int sound);


#endif
