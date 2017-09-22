#ifndef  UTILITY_INC
#define  UTILITY_INC

#include <string>

namespace Utility
{
    /// \name ʱ�亯��
    //@{

    /**
     * \brief ����ǰʱ���ַ������� fmtָ���ĸ�ʽ��ӡ��buf��
     * \return buf
     * \see man strftime
     */
    char * get_cur_time_formated(char * buf, size_t buf_size, const char * fmt);
    
    /**
     * \brief ����"0000-00-00 00:00:00"��ʽ��ʱ���ַ���
     */
    std::string get_standard_cur_timestr();

    std::string get_cur_4y2m2d();
    std::string get_cur_4y2m2d2h2m();
    std::string get_cur_4y2m2d2h2m2s();

    //@}
};

#endif   /* ----- #ifndef UTILITY_INC  ----- */

