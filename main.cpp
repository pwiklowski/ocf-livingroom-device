#include <QCoreApplication>
#include <sys/time.h>
#include "application.h"


uint64_t get_current_ms(){
    struct timeval te;
    gettimeofday(&te, NULL); // get current time
    long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000; // caculate milliseconds
    return milliseconds;
}



int main(int argc, char *argv[])
{
    Application a(argc, argv);

    return a.exec();
}

