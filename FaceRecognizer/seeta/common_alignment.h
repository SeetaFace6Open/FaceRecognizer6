#ifndef _SEETA_COMMON_ALIGNMENT_H
#define _SEETA_COMMON_ALIGNMENT_H
#include <cstdint>

/**
 * \brief ��������ʱ�������㷨
 */
enum SAMPLING_TYPE
{
	LINEAR,	///< ���Բ���
	BICUBIC	///< Cubic ����
};

/**
 * \brief ���ڱ�Եʱ�ı�Ե����㷨
 */
enum PADDING_TYPE
{
    ZERO_PADDING,           ///< 0 ���
    NEAREST_PADDING,        ///< �������
};

/**
 * \brief ͨ�������ü��ӿ�
 * \param image_data ����ͼ�� �� height * width * channels �Ĵ������ֽڱ�ʾ�ĻҶ�ֵ��0 �� 255��
 * \param image_width ͼƬ���
 * \param image_height ͼƬ�߶�
 * \param image_channels ͼƬͨ��������ɫͼƬһ��Ϊ 3���Ҷ�ͼƬһ��Ϊ 1
 * \param crop_data ���ͼ��Crop �õ����ݣ���[crop_width + pad_left + pad_right, crop_height + pad_top + pad_bottom, image_channels] ��С������
 * \param crop_width �����ȣ����ڱ�׼��������ȡ�Ŀ�ȣ�\b �������������ͼ��Ŀ�ȣ���pad����Ӱ��
 * \param crop_height ����߶ȣ����ڱ�׼��������ȡ�ĸ߶ȣ�\b �������������ͼ��ĸ߶ȣ���pad����Ӱ��
 * \param points ��λ�������㣬�� {(x1, y1), (x2, y2), ...} �ĸ�ʽ���
 * \param points_num ����������
 * \param mean_shape ƽ������ģ�ͣ��� {(x1, y1), (x2, y2), ...} �ĸ�ʽ���
 * \param mean_shape_width ƽ������ģ�͵Ŀ��
 * \param mean_shape_height ƽ������ģ�͵ĸ߶�
 * \param pad_top ������չ������Ϊ������ʾ��������
 * \param pad_bottom ������չ������Ϊ������ʾ��������
 * \param pad_left ������չ������Ϊ������ʾ��������
 * \param pad_right ������չ������Ϊ������ʾ��������
 * \param final_points �����ü����Ӧ����������꣬�� {(x1, y1), (x2, y2), ...} �ĸ�ʽ���
 * \param type ����ʱ��ֵʹ�õķ���
 * \return �����ü��Ƿ�ɹ�
 * \note ���ü�������������СΪ [crop_width + pad_left + pad_right, crop_height + pad_top + pad_bottom] �Ĵ�С
 */
bool face_crop_core(
	const uint8_t *image_data, int image_width, int image_height, int image_channels,
	uint8_t *crop_data, int crop_width, int crop_height,
	const float *points, int points_num,
	const float *mean_shape, int mean_shape_width, int mean_shape_height,
	int pad_top = 0, int pad_bottom = 0, int pad_left = 0, int pad_right = 0,
	float *final_points = nullptr,
	SAMPLING_TYPE type = LINEAR);

bool face_crop_core_ex(
    const uint8_t *image_data, int image_width, int image_height, int image_channels,
    uint8_t *crop_data, int crop_width, int crop_height,
    const float *points, int points_num,
    const float *mean_shape, int mean_shape_width, int mean_shape_height,
    int pad_top = 0, int pad_bottom = 0, int pad_left = 0, int pad_right = 0,
    float *final_points = nullptr,
    SAMPLING_TYPE type = LINEAR,
    PADDING_TYPE ptype = ZERO_PADDING);

#endif // _SEETA_COMMON_ALIGNMENT_H
