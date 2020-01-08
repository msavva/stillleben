"""
Loss functions to be used with stillleben differentiation package.
"""

def neg_iou_loss(predict, target):
    r"""
    Negative intersection over union loss function
    Args:
        predict (tensor): BxCxHxW image
        target (tensor): BxCxHxW image
    """
    dims = tuple(range(predict.ndimension())[1:])
    _intersect = predict * target
    intersect = (_intersect).sum(dims)
    _union = predict + target - predict * target
    union = (_union).sum(dims) + 1e-6
    loss_img = (1. - (_intersect / (_union + 1e-6) )).detach().clone()
    
    return 1. - (intersect / union).sum() / intersect.nelement(), loss_img