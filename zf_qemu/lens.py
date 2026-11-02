"""Settings for the declared, experimental synthetic lens input."""
APERTURES = (100, 120, 140, 160, 180, 200, 220, 250, 280, 320, 350,
             400, 500, 560, 630, 710, 800, 900, 1000, 1100, 1300, 1400, 1600)
DEFAULT = {'focal_length_mm': 50, 'aperture_x100': 100}


def validate(value):
    focal, aperture = value.get('focal_length_mm'), value.get('aperture_x100')
    if type(focal) is not int or not 6 <= focal <= 1200:
        raise ValueError('焦距必须为 6–1200 mm 的整数')
    if type(aperture) is not int or aperture not in APERTURES:
        raise ValueError('光圈必须选用列表中的档位')
    return {'focal_length_mm': focal, 'aperture_x100': aperture}


def qom_value(settings):
    settings = validate(settings)
    return f'{settings["focal_length_mm"]}:{settings["aperture_x100"]}'
