1. ����
git���ύ���룬�����tag��
��������µ�/opt/rpmbuild/SOURCES/src/live_stream_svr��
��tag��ʽΪ��rtp-3.3.������.count������rtp-3.3.170424.1��

2. ���벢���receiver��forward
cd /opt/rpmbuild
./build_receiver_rtp.sh 3.3.170424.1

3. ����
cd /root
vi dep-rpm.sh
�޸�NEWVER
./dep-rpm.sh