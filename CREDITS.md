# Авторы и лицензии

## Демо-игра «Вопрос»

Демо-игра перенесена из *The Question* — примера, который входит в поставку
[Ren'Py](https://github.com/renpy/renpy). По лицензии Ren'Py ассеты демо
распространяются на тех же условиях, что и сам движок Ren'Py (MIT):
«The artwork in the demo is released by various copyright holders, under
the same terms».

Авторы оригинала, как они указаны в самой игре:

- Character Art: Deji
- Original Character Art: derik
- Background Art: Mugenjohncel
- Original Background Art: DaFool
- Written By: mikey

Русский перевод взят из той же поставки Ren'Py (`the_question/game/tl/russian`).

В репозитории лежат производные версии картинок: фоны уменьшены и обрезаны
до 320×240, спрайты уменьшены втрое. Тексты сценария перенесены в язык
движка инструментом `tools/import_renpy.py`.

Музыка оригинала (Alessio) в репозиторий **не входит**. Сцена дождя
(`art/scene/rain`), титульный экран (`art/bg/title.png`), чёрный фон,
иконка сохранения и музыкальная тема (`music/theme.mid`) сделаны для этого
репозитория на основе перечисленных выше ассетов или с нуля.

Текст лицензии MIT, под которой распространяются ассеты демо:

```
Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Шрифт

Шрифт растеризуется при сборке из DejaVu Sans, установленного в системе;
сам файл шрифта в репозиторий не входит. DejaVu распространяется под
свободной лицензией (Bitstream Vera с дополнениями), подробности — на
[dejavu-fonts.github.io](https://dejavu-fonts.github.io/License.html).

## Файл лицензии Sony

`licensea.dat` — лицензионный блок, который BIOS приставки проверяет при
загрузке, — в репозиторий **не входит** и распространяться не может.
Как с этим быть — в [docs/building.md](docs/building.md).
